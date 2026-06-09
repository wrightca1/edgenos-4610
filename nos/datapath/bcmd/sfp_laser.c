/*
 * sfp_laser.c — standalone SFP+ 84758 laser/optical bring-up for the AS4610-54T,
 * driven over the iProc CCB MDIO master (0x18032000) via /dev/mem.
 *
 * WHY standalone: the BCM84758 SFP+ PHY sits on the iProc CCB MDIO bus, NOT the
 * chip's CMIC SBUS MIIM that the OpenBCM SDK (bcmd) probes — so bcmd never reaches
 * it and the optic's TX laser stays hard-disabled (TX_DISABLE/c800[7]). The iProc
 * MDIO is independent of the chip BDE, so this one-shot can enable the laser +
 * optical config WHILE bcmd owns the datapath.
 *
 * The register sequence is edged's sfp_tx_enable() (nos/datapath/mdk-app/edged.c),
 * which writes the ICOS LOCKED values (c800=0x3f3f, c8e4=0xc8ef, c805 optical,
 * 10G PMA, PCS un-reset, clear TX_DISABLE, TxOnOff strap toggle). Those values were
 * captured from a working ICOS 84758 (dumps/icos_linked_2026_06_06/21_*).
 *
 * Build: arm-linux-gnueabihf-gcc -O2 -o sfp_laser sfp_laser.c
 * Run:   ./sfp_laser            (all 4 SFP+ ports xe0-3 = MDIO 0x40-0x43)
 *        ./sfp_laser 0          (just xe0)
 * Verify: read the optic DOM TX power afterwards (should go > 0).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

/* iProc CCB MDIO master (from ONL DTS / mdio-xgs-iproc-cc + edged.c) */
#define IPROC_MDIO_MGMT_BASE  0x18032000   /* [0]=MGMT_CTL [1]=MGMT_CMD_DATA */
#define IPROC_MDIO_EN_BASE    0x1803f000
#define IPROC_MDIO_EN_OFF     0xc3c        /* enable reg = 0x1803fc3c */
#define IPROC_MDIO_EN_BIT     4
#define MC_PRE   (1u << 7)
#define MC_BSY   (1u << 8)
#define MC_EXT   (1u << 9)
#define CD_SB(x)   ((uint32_t)(x) << 30)
#define CD_OP(x)   ((uint32_t)(x) << 28)
#define CD_PA(x)   ((uint32_t)(x) << 23)
#define CD_RA(x)   ((uint32_t)(x) << 18)
#define CD_TA(x)   ((uint32_t)(x) << 16)
#define CD_DATA(x) ((uint32_t)(x) & 0xffff)

/* 84758 (BCM84740 family) register map — from edged.c X84_* defines */
#define X84_OPT_CFG     0xc8e4   /* optical cfg: b12 TxOn(0=en), b4 TX-active-state */
#define X84_OPT_SIGLVL  0xc800   /* optical sig level / laser (b7 strap), ICOS=0x3f3f */
#define X84_TX_DISABLE  0x0009   /* PMD global TX disable (b0) */
#define X84_GENSIG_8071 0xcd16   /* GENSIG TX disable (b3) */
#define X84_AER_ADDR    0xc702   /* lane address-extension */
#define X84_PMAD_CTRL   0x0000   /* PMA/PMD ctrl1: speed-select */
#define X84_PMAD_CTRL2  0x0007   /* PMA/PMD ctrl2: PMA type (low nibble) */
#define X84_SS_10G      0x2040
#define X84_PMA_TYPE_10G_LRM 0x8
#define X84_RESET_CTRL  0xcd17   /* clear to un-reset the media PCS */
#define X84_SIDE_SEL    0xffff   /* 1.0xffff b0: 0=MMF/line(optics) 1=XFI/system */
#define X84_CHIP_MODE   0xc805   /* b3 DAC, b2 backplane (clear both=optical) */
#define X84_DAC_MODE    (1u << 3)
#define X84_BKPLANE_MODE (1u << 2)
#define X84_REPEATER_DET 0xc81d
#define X84_SQUELCH_CTL 0xcd18

static volatile uint32_t *mgmt;   /* MGMT_CTL/CMD_DATA */
static volatile uint32_t *enpg;
static int g_ext = 1;             /* MC_EXT: 1=external bus, 0=internal */

static void *mapmem(off_t p)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    void *v;
    if (fd < 0) { perror("open /dev/mem"); return NULL; }
    v = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, p);
    close(fd);
    if (v == MAP_FAILED) { perror("mmap"); return NULL; }
    return v;
}

static int mdio_wait(void)
{
    int us = 0;
    while (mgmt[0] & MC_BSY) {
        if (us > 1000) return -1;
        usleep(10); us += 10;
    }
    return 0;
}

/* clause-45 (ST=00) read over the external iProc MDIO. */
static int c45_read(int phy, int devad, uint16_t reg)
{
    if (mdio_wait() < 0) return -1;
    if (g_ext) mgmt[0] |= MC_EXT; else mgmt[0] &= ~MC_EXT;
    mgmt[1] = CD_SB(0)|CD_OP(0)|CD_PA(phy)|CD_RA(devad)|CD_TA(2)|CD_DATA(reg); /* set addr */
    if (mdio_wait() < 0) return -1;
    mgmt[1] = CD_SB(0)|CD_OP(3)|CD_PA(phy)|CD_RA(devad)|CD_TA(2);              /* read */
    if (mdio_wait() < 0) return -1;
    return (int)(mgmt[1] & 0xffff);
}

/* clause-45 write over the external iProc MDIO (OP=1=write). */
static int c45_write(int phy, int devad, uint16_t reg, uint16_t val)
{
    if (mdio_wait() < 0) return -1;
    if (g_ext) mgmt[0] |= MC_EXT; else mgmt[0] &= ~MC_EXT;
    mgmt[1] = CD_SB(0)|CD_OP(0)|CD_PA(phy)|CD_RA(devad)|CD_TA(2)|CD_DATA(reg); /* set addr */
    if (mdio_wait() < 0) return -1;
    mgmt[1] = CD_SB(0)|CD_OP(1)|CD_PA(phy)|CD_RA(devad)|CD_TA(2)|CD_DATA(val); /* write */
    if (mdio_wait() < 0) return -1;
    return 0;
}

/* clause-22 read (SB=1, OP=2) — for the 54282 sanity check. */
static int c22_read(int phy, int reg)
{
    if (mdio_wait() < 0) return -1;
    if (g_ext) mgmt[0] |= MC_EXT; else mgmt[0] &= ~MC_EXT;
    mgmt[1] = CD_SB(1)|CD_OP(2)|CD_PA(phy)|CD_RA(reg)|CD_TA(2);
    if (mdio_wait() < 0) return -1;
    return (int)(mgmt[1] & 0xffff);
}

/* Scan both EXT modes + all addresses for the 84758 (C45 ID 0x600d:0x86fx) and
 * the 54282 (C22 ID 0x600d:0x845x, proves the bus works). Returns 84758 addr or -1. */
static int scan_phys(void)
{
    int e, phy, id0, id1, found = -1;
    for (e = 1; e >= 0; e--) {
        g_ext = e;
        printf("[scan] EXT=%d:\n", e);
        for (phy = 0; phy < 32; phy++) {
            id0 = c45_read(phy, 1, 0x0002); id1 = c45_read(phy, 1, 0x0003);
            if (id0 > 0 && id0 != 0xffff)
                printf("   C45 phy 0x%02x: %04x:%04x%s\n", phy, id0, id1,
                       (id0==0x600d && (id1&~0xf)==0x86f0) ? "  <== BCM84758" : "");
            if (id0==0x600d && (id1&~0xf)==0x86f0 && found<0) found = phy;
            id0 = c22_read(phy, 2); id1 = c22_read(phy, 3);
            if (id0 > 0 && id0 != 0xffff)
                printf("   C22 phy 0x%02x: %04x:%04x%s\n", phy, id0, id1,
                       (id0==0x600d && (id1&~0xf)==0x845b) ? "  <== BCM54282" : "");
        }
    }
    g_ext = 1;
    return found;
}

/* read-modify-write a c45 reg: clear `clr` bits, set `set` bits. */
static void c45_rmw(int phy, int devad, uint16_t reg, uint16_t clr, uint16_t set)
{
    int v = c45_read(phy, devad, reg);
    if (v < 0) return;
    c45_write(phy, devad, reg, (uint16_t)((v & ~clr) | set));
}

/* Enable the laser + optical config on one SFP+ port (84758 at MDIO `phy`).
 * Mirrors edged sfp_tx_enable() using the ICOS LOCKED values. devad 1 = PMA/PMD
 * unless noted (7=AN, 3=PCS). */
static void sfp_port_enable(int phy)
{
    int v;
    c45_write(phy, 1, X84_AER_ADDR, 0);                       /* lane 0 */
    c45_rmw(phy, 1, X84_SIDE_SEL, 0x1, 0);                    /* MMF/line side */
    c45_rmw(phy, 1, X84_CHIP_MODE, X84_DAC_MODE|X84_BKPLANE_MODE, 0); /* optical */
    c45_rmw(phy, 7, 0x0000, (1u<<12), 0);                     /* AN_EN off */
    c45_write(phy, 1, 0x0096, 0);                             /* CL72 off */
    c45_write(phy, 1, X84_PMAD_CTRL, X84_SS_10G);             /* 10G speed */
    c45_rmw(phy, 1, X84_PMAD_CTRL2, 0xf, X84_PMA_TYPE_10G_LRM); /* PMA type LRM */
    c45_write(phy, 1, X84_RESET_CTRL, 0);                     /* un-reset media PCS */
    c45_write(phy, 1, X84_OPT_CFG, 0xc8ef);                   /* ICOS locked */
    c45_write(phy, 1, X84_OPT_SIGLVL, 0x3f3f);               /* ICOS locked — LASER */
    c45_write(phy, 1, 0xc82b, 0x8a40);                        /* ICOS locked */
    c45_rmw(phy, 1, X84_TX_DISABLE, 0x1, 0);                  /* clear PMD TX disable */
    c45_rmw(phy, 1, X84_GENSIG_8071, 0x8, 0);                 /* clear GENSIG TX disable */
    c45_rmw(phy, 1, X84_OPT_CFG, (1u<<12), 0);                /* TxOn enable */
    /* TxOnOff strap: c8e4[4] = TxOnOff_pin XOR c800[7]; if b4=1 toggle c800[7]. */
    v = c45_read(phy, 1, X84_OPT_CFG);
    if (v >= 0 && (v & (1u<<4))) {
        int c800 = c45_read(phy, 1, X84_OPT_SIGLVL);
        if (c800 >= 0)
            c45_write(phy, 1, X84_OPT_SIGLVL,
                      (uint16_t)((c800 & (1u<<7)) ? (c800 & ~(1u<<7)) : (c800 | (1u<<7))));
    }
    /* repeater squelch-enable (system side) if in retimer mode */
    v = c45_read(phy, 1, X84_REPEATER_DET);
    if (v >= 0 && (v & 0x6) == 0x6) {
        c45_write(phy, 1, X84_SIDE_SEL, 1);                  /* system side */
        c45_rmw(phy, 3, X84_SQUELCH_CTL, 0, (1u<<4)|(1u<<6));
        c45_write(phy, 1, X84_SIDE_SEL, 0);                  /* back to MMF */
    }
    printf("  mdio 0x%02x: c800=0x%04x c8e4=0x%04x  (laser-on target c800=0x3f3f)\n",
           phy, c45_read(phy, 1, X84_OPT_SIGLVL), c45_read(phy, 1, X84_OPT_CFG));
}

int main(int argc, char **argv)
{
    uint32_t div;
    int lo = 0, hi = 3, i;

    if (argc > 1) { lo = hi = atoi(argv[1]); }

    mgmt = mapmem(IPROC_MDIO_MGMT_BASE);
    enpg = mapmem(IPROC_MDIO_EN_BASE);
    if (!mgmt || !enpg) return 1;

    enpg[IPROC_MDIO_EN_OFF / 4] |= (1u << IPROC_MDIO_EN_BIT);  /* enable external MDIO */
    div = mgmt[0] & 0x7f; if (div == 0) div = 0x32;
    mgmt[0] = MC_PRE | div;

    printf("[sfp_laser] iProc CCB MDIO @0x18032000: MGMT_CTL=0x%08x en=0x%08x\n",
           mgmt[0], enpg[IPROC_MDIO_EN_OFF/4]);

    if (argc > 1 && !strcmp(argv[1], "scan")) {
        int a = scan_phys();
        printf("[scan] 84758 %s\n", a >= 0 ? "FOUND" : "NOT found");
        if (a >= 0) printf("[scan] 84758 at addr 0x%02x (EXT=1)\n", a);
        return 0;
    }
    if (argc > 1) { lo = hi = atoi(argv[1]); }  /* explicit MDIO addr */
    else { lo = 0; hi = 3; }

    printf("[sfp_laser] enabling SFP+ laser/optics at MDIO addr 0x%02x-0x%02x\n", lo, hi);
    for (i = lo; i <= hi; i++) {
        sfp_port_enable(i);
    }
    printf("[sfp_laser] done. Check the optic DOM TX power (should be > 0).\n");
    return 0;
}
