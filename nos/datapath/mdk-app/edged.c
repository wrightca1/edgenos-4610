/*
 * edged.c — EdgeNOS-4610 switch data-plane daemon.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 EdgeNOS project.
 *
 * This is OUR code (open source, BSD-3-Clause). It links the Broadcom OpenMDK
 * CDK/BMD/PHY libraries (source-available; see ../../../LICENSING.md). A
 * permissive license is used deliberately so edged can lawfully link the
 * GPL-incompatible OpenMDK SDK.
 *
 * edged brings the BCM56340 (Helix4) data plane up on the AS4610-54T by driving
 * the OpenMDK BMD over the on-die CMIC, mapped via /dev/mem at phys 0x48000000
 * (no Broadcom kernel module; STRICT_DEVMEM is off in the ONL kernel).
 *
 * Bring-up order (proven on hardware 2026-06-05):
 *     attach -> bmd_reset -> bmd_init -> bmd_switching_init
 * bmd_reset MUST precede bmd_init: reset sets up TOP_CORE_PLL + core clocks;
 * init then talks to the core over SCHAN. Skipping reset => "S-channel error /
 * IPIPE reset timeout".
 *
 * Stages: attach -> reset -> init -> swinit -> port config (native speed) ->
 * L2 STP-forwarding -> port inventory. With --keep, a control loop monitors
 * link state and re-syncs the MAC on link changes (bmd_port_mode_update).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <linux/i2c-dev.h>

/* The SMBus union + protocol constants live in <linux/i2c.h> on modern
 * sysroots (split out of <linux/i2c-dev.h>). Pull it in if available; define
 * the few bits we need as a fallback so the CPLD path builds on any toolchain. */
#if defined(__has_include)
#  if __has_include(<linux/i2c.h>)
#    include <linux/i2c.h>
#  endif
#endif
#ifndef I2C_SMBUS_BYTE_DATA
/* <linux/i2c.h> absent on this toolchain: define the SMBus byte-data ABI
 * ourselves (the I2C_SMBUS ioctl number + i2c_smbus_ioctl_data struct still
 * come from <linux/i2c-dev.h>, which is always present). */
#define I2C_SMBUS_WRITE     0
#define I2C_SMBUS_READ      1
#define I2C_SMBUS_BYTE_DATA 2
union i2c_smbus_data {
    unsigned char  byte;
    unsigned short word;
    unsigned char  block[34];
};
#endif

#include <cdk_config.h>
#include <cdk/cdk_device.h>
#include <cdk/cdk_chip.h>
#include <cdk/arch/xgsm_miim.h>

#include <bmd_config.h>
#include <bmd/bmd.h>
#include <bmd/bmd_phy.h>
#include <bmd/bmd_phy_ctrl.h>
#include <bmd/bmd_device.h>   /* BMD_PORT_STATUS / BMD_PST_FORCE_LINK (SFP+ link force) */

#include <phy_config.h>
#include <phy/phy.h>
#include <phy/phy_drvlist.h>
#include <phy/phy_aer_iblk.h>

#include "linux_shbde.h"
#include "edged_l3.h"
#include <bmd/bmd_dma.h>          /* bmd_dma_alloc_coherent (coherent pool via BDE) */
#include <bmdi/arch/xgsm_dma.h>   /* bmd_xgsm_dma_init (CMICm DMA channel setup) */

/* AS4610-54T default: BCM56340 (Helix4) CMIC at on-die phys 0x48000000. */
#define EDGED_DEFAULT_UNIT  "0x14e4:0xb340:0x01@0x48000000"
#define CMIC_WINDOW_BYTES   (256 * 1024)
#define MAX_FRONT_PORTS     128
#define LINK_POLL_SEC       5

#define LOG(fmt, ...)  fprintf(stderr, "edged: " fmt "\n", ##__VA_ARGS__)

#if !defined(SYS_BE_PIO) || !defined(SYS_BE_PACKET) || !defined(SYS_BE_OTHER)
#error "bus endian flags SYS_BE_PIO/PACKET/OTHER not defined (pass -DSYS_BE_*=0 for armhf)"
#endif

static int front_ports[MAX_FRONT_PORTS];
static int n_front;
static int link_state[MAX_FRONT_PORTS];   /* indexed by front_ports[] slot */

/* SFP+ logical ports (cfg 400: the BCM84758 binds at logical 53-56). */
#define SFP_LO 53
#define SFP_HI 56

/* Per-port dynamic config (CDK DYN_CONFIG): a small set of speed classes that the
 * SFP+ override hangs off. bcm56340_a0_port_speed_max() consults this (via
 * cdk_dev_port_speed_max_get) whenever num_port_configs != 0, BEFORE the static
 * cfg-400 table — so installing it lets us report 10G for the SFP+ ports and have
 * bmd_port_mode_set accept a forced 10G mode (driving the Warpcore SerDes + 84758
 * to 10G). port_config_id is a char, so we dedup speeds into <=16 classes. */
static cdk_port_config_t edged_pcfg[16];
static int edged_npcfg;

static int
edged_pcfg_id(uint32_t speed_max)
{
    int i;
    for (i = 0; i < edged_npcfg; i++) {
        if (edged_pcfg[i].speed_max == speed_max) {
            return i;
        }
    }
    if (edged_npcfg < (int)(sizeof(edged_pcfg) / sizeof(edged_pcfg[0]))) {
        edged_pcfg[edged_npcfg].speed_max  = speed_max;
        edged_pcfg[edged_npcfg].port_flags = 0;
        edged_pcfg[edged_npcfg].port_mode  = CDK_DCFG_PORT_MODE_IEEE;
        edged_pcfg[edged_npcfg].sys_port   = 0;
        edged_pcfg[edged_npcfg].app_port   = 0;
        return edged_npcfg++;
    }
    return 0;
}

/* BMD/PHY sleep hook (referenced via -DBMD_SYS_USLEEP=_usleep). */
int
_usleep(uint32_t usecs)
{
    return usleep(usecs);
}

/* mmap a physical region of /dev/mem into our address space. */
static uint32_t *
_mmap(off_t p, int size)
{
    static int memfd = -1;
    uint32_t *va;

    if (memfd == -1) {
        if ((memfd = open("/dev/mem", O_RDWR | O_SYNC | O_DSYNC | O_RSYNC)) < 0) {
            perror("edged: open /dev/mem");
            return NULL;
        }
    }
    va = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, p);
    if (va == NULL || va == MAP_FAILED) {
        fprintf(stderr, "edged: mmap phys 0x%x failed: ", (unsigned int)p);
        perror("");
        return NULL;
    }
    return va;
}

/* AS4610 is little-endian ARM: no PIO/packet/other byte-swap. */
static uint32_t
_bus_flags(void)
{
    uint32_t flags = CDK_DEV_MBUS_PCI;
#if SYS_BE_PIO == 1
    flags |= CDK_DEV_BE_PIO;
#endif
#if SYS_BE_PACKET == 1
    flags |= CDK_DEV_BE_PACKET;
#endif
#if SYS_BE_OTHER == 1
    flags |= CDK_DEV_BE_OTHER;
#endif
    return flags;
}

/* Create a CDK device context for the CMIC mapped at base_addr. */
static int
_create_cdk_device(uint32_t vendor_id, uint32_t device_id, uint32_t rev_id,
                   uint32_t base_addr)
{
    cdk_dev_id_t id;
    cdk_dev_vectors_t dv;
    int rc;

    memset(&id, 0, sizeof(id));
    id.vendor_id = vendor_id;
    id.device_id = device_id;
    id.revision  = rev_id;

    memset(&dv, 0, sizeof(dv));
    dv.base_addr = _mmap(base_addr, CMIC_WINDOW_BYTES);
    if (dv.base_addr == NULL) {
        return -1;
    }

    rc = cdk_dev_create(&id, &dv, _bus_flags());
    if (rc < 0) {
        LOG("cdk_dev_create failed for 0x%x:0x%x:0x%x @ 0x%x: %s (%d)",
            vendor_id, device_id, rev_id, base_addr, CDK_ERRMSG(rc), rc);
    }
    return rc;
}

/* Map a resolved port mode to a coarse speed label for the inventory. */
static const char *
mode_str(bmd_port_mode_t m)
{
    switch (m) {
    case bmdPortModeAuto:       return "auto";
    case bmdPortModeDisabled:   return "disabled";
    case bmdPortMode10fd: case bmdPortMode10hd:      return "10M";
    case bmdPortMode100fd: case bmdPortMode100hd: case bmdPortMode100FX: return "100M";
    case bmdPortMode1000fd: case bmdPortMode1000hd:
    case bmdPortMode1000X: case bmdPortMode1000KX:   return "1G";
    case bmdPortMode2500fd:     return "2.5G";
    case bmdPortMode5000fd:     return "5G";
    case bmdPortMode10000fd: case bmdPortMode10000SFI: case bmdPortMode10000XFI:
    case bmdPortMode10000CR: case bmdPortMode10000KR: case bmdPortMode10000KX:
                                return "10G";
    case bmdPortMode20000fd: case bmdPortMode20000KR: case bmdPortMode21000fd:
                                return "20G";
    case bmdPortMode40000fd: case bmdPortMode40000CR: case bmdPortMode40000KR:
    case bmdPortMode42000fd:    return "40G";
    default:                    return "?";
    }
}

/* ---- iProc CCB MDIO master (the EXTERNAL PHYs live here, NOT on the CMIC
 *      SBUS MIIM): BCM84758 10G SFP+ and BCM54282 1G. From the ONL DTS +
 *      Linux mdio-xgs-iproc-cc driver: brcm,iproc-ccb-mdio @ 0x18032000,
 *      enable bit at 0x1803fc3c bit 4. ---- */
#define IPROC_MDIO_MGMT_BASE  0x18032000   /* MGMT_CTL@+0, MGMT_CMD_DATA@+4 */
#define IPROC_MDIO_EN_BASE    0x1803f000   /* page holding the enable reg */
#define IPROC_MDIO_EN_OFF     0xc3c        /* enable reg = 0x1803fc3c */
#define IPROC_MDIO_EN_BIT     4            /* DT iproc-mdio-sel-bit */
#define MC_PRE   (1u << 7)                 /* MGMT_CTL preamble */
#define MC_BSY   (1u << 8)                 /* MGMT_CTL busy */
#define MC_EXT   (1u << 9)                 /* MGMT_CTL external-bus select */
#define CD_SB(x)   ((uint32_t)(x) << 30)
#define CD_OP(x)   ((uint32_t)(x) << 28)
#define CD_PA(x)   ((uint32_t)(x) << 23)
#define CD_RA(x)   ((uint32_t)(x) << 18)
#define CD_TA(x)   ((uint32_t)(x) << 16)
#define CD_DATA(x) ((uint32_t)(x) & 0xffff)

static volatile uint32_t *iproc_mgmt;   /* [0]=MGMT_CTL [1]=MGMT_CMD_DATA */
static volatile uint32_t *iproc_enpg;
static int g_iproc_ext = 1;             /* MGMT_CTL EXT bit (1=external bus) */

static void
iproc_set_ext(void)
{
    if (g_iproc_ext) {
        iproc_mgmt[0] |= MC_EXT;
    } else {
        iproc_mgmt[0] &= ~MC_EXT;
    }
}

static int
iproc_mdio_init(void)
{
    uint32_t ctl, div;
    iproc_mgmt = (volatile uint32_t *)_mmap(IPROC_MDIO_MGMT_BASE, 4096);
    iproc_enpg = (volatile uint32_t *)_mmap(IPROC_MDIO_EN_BASE, 4096);
    if (iproc_mgmt == NULL || iproc_enpg == NULL) {
        return -1;
    }
    /* Enable the external MDIO master (set the sel bit). */
    iproc_enpg[IPROC_MDIO_EN_OFF / 4] |= (1u << IPROC_MDIO_EN_BIT);
    /* MGMT_CTL: keep existing MDC divider if set, else a conservative value;
     * preamble on. EXT is set per transaction. */
    ctl = iproc_mgmt[0];
    div = ctl & 0x7f;
    if (div == 0) {
        div = 0x32;
    }
    iproc_mgmt[0] = MC_PRE | div;
    return 0;
}

static int
iproc_mdio_wait(void)
{
    int us = 0;
    while (iproc_mgmt[0] & MC_BSY) {
        if (us > 500) {
            return -1;
        }
        usleep(10);
        us += 10;
    }
    return 0;
}

/* Clause-45 read over the iProc CCB MDIO (external bus). sb/read_op let us probe
 * the exact framing the controller wants. Returns -1 on error, else 16-bit val. */
static int
iproc_mdio_c45_read_ex(int phy, int devad, uint16_t reg, uint32_t sb, uint32_t read_op)
{
    if (iproc_mdio_wait() < 0) {
        return -1;
    }
    iproc_set_ext();
    /* address frame: OP=0 (set C45 address), TA=2 */
    iproc_mgmt[1] = CD_SB(sb) | CD_OP(0) | CD_PA(phy) | CD_RA(devad) |
                    CD_TA(2) | CD_DATA(reg);
    if (iproc_mdio_wait() < 0) {
        return -1;
    }
    /* read frame: OP=read_op, TA=2 */
    iproc_mgmt[1] = CD_SB(sb) | CD_OP(read_op) | CD_PA(phy) | CD_RA(devad) | CD_TA(2);
    if (iproc_mdio_wait() < 0) {
        return -1;
    }
    return (int)(iproc_mgmt[1] & 0xffff);
}

static int
iproc_mdio_c45_read(int phy, int devad, uint16_t reg)
{
    return iproc_mdio_c45_read_ex(phy, devad, reg, 0, 3);
}

/* Clause-22 read over the iProc CCB MDIO (external bus): SB=1, OP=2 (read). */
static int
iproc_mdio_c22_read(int phy, int reg)
{
    if (iproc_mdio_wait() < 0) {
        return -1;
    }
    iproc_set_ext();
    iproc_mgmt[1] = CD_SB(1) | CD_OP(2) | CD_PA(phy) | CD_RA(reg) | CD_TA(2);
    if (iproc_mdio_wait() < 0) {
        return -1;
    }
    return (int)(iproc_mgmt[1] & 0xffff);
}

/* Scan the iProc external MDIO for PHYs (the 84758 is C45; 54282 is C22). */
static void
scan_iproc_mdio(void)
{
    int phy, id0, id1;

    if (iproc_mdio_init() < 0) {
        LOG("iProc MDIO: mmap failed (need root)");
        return;
    }
    LOG("iProc CCB MDIO @ 0x18032000: MGMT_CTL=0x%08x  enable[0x..fc3c]=0x%08x",
        iproc_mgmt[0], iproc_enpg[IPROC_MDIO_EN_OFF / 4]);

    LOG("iProc ext scan — C45 (devad1 reg2/3) for 84758:");
    for (phy = 0; phy < 32; phy++) {
        id0 = iproc_mdio_c45_read(phy, 1, 0x0002);
        id1 = iproc_mdio_c45_read(phy, 1, 0x0003);
        if (id0 < 0 || id1 < 0) continue;
        if ((id0 == 0 || id0 == 0xffff) && (id1 == 0 || id1 == 0xffff)) continue;
        LOG("  C45 phy 0x%02x: ID %04x:%04x%s", phy, id0, id1,
            (id0 == 0x600d && (id1 & ~0xf) == 0x86f0) ? "  <== BCM84758" : "");
    }
    LOG("iProc ext scan — C22 (reg2/3) for 54282 (sanity that the bus works):");
    for (phy = 0; phy < 32; phy++) {
        id0 = iproc_mdio_c22_read(phy, 0x02);
        id1 = iproc_mdio_c22_read(phy, 0x03);
        if (id0 < 0 || id1 < 0) continue;
        if ((id0 == 0 || id0 == 0xffff) && (id1 == 0 || id1 == 0xffff)) continue;
        LOG("  C22 phy 0x%02x: ID %04x:%04x%s", phy, id0, id1,
            (id0 == 0x600d && (id1 & ~0xf) == 0x845b) ? "  <== BCM54282" : "");
    }
    /* Second pass on the INTERNAL selection (EXT=0): the front-panel 0x600d
     * PHYs (54282 1G / 84758 10G) may hang off this segment of the master. */
    g_iproc_ext = 0;
    LOG("iProc INTERNAL (EXT=0) scan — C22 + C45, phy 0-31:");
    for (phy = 0; phy < 32; phy++) {
        id0 = iproc_mdio_c22_read(phy, 0x02);
        id1 = iproc_mdio_c22_read(phy, 0x03);
        if (!(id0 < 0 || id1 < 0) &&
            !((id0 == 0 || id0 == 0xffff) && (id1 == 0 || id1 == 0xffff))) {
            LOG("  int C22 phy 0x%02x: ID %04x:%04x%s", phy, id0, id1,
                (id0 == 0x600d && (id1 & ~0xf) == 0x845b) ? "  <== BCM54282" : "");
        }
        id0 = iproc_mdio_c45_read(phy, 1, 0x0002);
        id1 = iproc_mdio_c45_read(phy, 1, 0x0003);
        if (!(id0 < 0 || id1 < 0) &&
            !((id0 == 0 || id0 == 0xffff) && (id1 == 0 || id1 == 0xffff))) {
            LOG("  int C45 phy 0x%02x: ID %04x:%04x%s", phy, id0, id1,
                (id0 == 0x600d && (id1 & ~0xf) == 0x86f0) ? "  <== BCM84758" : "");
        }
    }
    g_iproc_ext = 1;

    /* Focused debug on the PHY we found (addr 1 = mgmt PHY): C22 register
     * dump + C45 framing variants, raw (unfiltered) values. */
    {
        int r;
        LOG("phy1 raw C22 regs 0-5:");
        for (r = 0; r <= 5; r++) {
            LOG("    c22 reg%d = 0x%04x", r, iproc_mdio_c22_read(1, r) & 0xffff);
        }
        LOG("phy1 C45 devad1 reg2 framings:");
        LOG("    SB0/OP3 = 0x%04x", iproc_mdio_c45_read_ex(1, 1, 2, 0, 3) & 0xffff);
        LOG("    SB0/OP2 = 0x%04x", iproc_mdio_c45_read_ex(1, 1, 2, 0, 2) & 0xffff);
        LOG("    SB2/OP3 = 0x%04x", iproc_mdio_c45_read_ex(1, 1, 2, 2, 3) & 0xffff);
        LOG("    devad1 reg3 SB0/OP3 = 0x%04x", iproc_mdio_c45_read_ex(1, 1, 3, 0, 3) & 0xffff);
    }
    LOG("iProc CCB MDIO scan done");
}

extern int cdk_xgsm_miim_read(int unit, uint32_t phy_addr, uint32_t reg, uint32_t *val);

/* Replay ICOS's CMIC MIIM bus-map / select registers (RE'd by booting ICOS and
 * dumping 0x48011000 while the external MDIO worked — see dumps/icos_mdio_regs.txt).
 * Our bmd_init leaves these 0, so external MDIO is unreachable; ICOS sets them to
 * route the external buses. This is the missing external-MDIO enable. */
/* SMBus byte-data primitives (struct i2c_smbus_ioctl_data from <linux/i2c-dev.h>).
 * We talk to the CPLD with SMBus write/read-byte-data — the SAME transaction type
 * the ONL accton_as4610_cpld kernel driver uses — instead of a raw 2-byte write().
 * The raw write() raced with the bound driver's periodic polling and silently
 * dropped some registers (0x0d/0x19/0x1b in the prior run); SMBus + retry + a
 * read-back verify makes the deassert deterministic from a clean boot. */
static int
cpld_smbus_xfer(int fd, int rw, uint8_t reg, union i2c_smbus_data *data)
{
    struct i2c_smbus_ioctl_data args;
    args.read_write = (uint8_t)rw;
    args.command    = reg;
    args.size       = I2C_SMBUS_BYTE_DATA;
    args.data       = data;
    return ioctl(fd, I2C_SMBUS, &args);
}

static int
cpld_read_fd(int fd, uint8_t reg)
{
    union i2c_smbus_data data;
    if (cpld_smbus_xfer(fd, I2C_SMBUS_READ, reg, &data) < 0) return -1;
    return data.byte & 0xff;
}

/* Write one CPLD (i2c0 @ 0x30) register, with retries and read-back verify.
 * FORCE because the ONL accton_as4610_cpld driver is bound to the device. */
static void
cpld_write(int reg, int val)
{
    int fd = open("/dev/i2c-0", O_RDWR);
    int tries;
    if (fd < 0) { LOG("cpld: open /dev/i2c-0 failed: %s", strerror(errno)); return; }
    if (ioctl(fd, I2C_SLAVE_FORCE, 0x30) < 0) {
        LOG("cpld: I2C_SLAVE_FORCE 0x30 failed: %s", strerror(errno));
        close(fd);
        return;
    }
    for (tries = 0; tries < 8; tries++) {
        union i2c_smbus_data data;
        data.byte = (uint8_t)val;
        if (cpld_smbus_xfer(fd, I2C_SMBUS_WRITE, (uint8_t)reg, &data) >= 0) {
            int rb = cpld_read_fd(fd, (uint8_t)reg);
            /* Accept if read-back matches, or if the reg is unreadable (-1):
             * some CPLD regs are write-only/strobe; the write itself succeeded. */
            if (rb < 0 || rb == (val & 0xff)) { close(fd); return; }
        }
        usleep(3000);   /* let the bound driver's poll finish, then retry */
    }
    LOG("cpld: write 0x%02x=0x%02x failed after retries (last errno: %s)",
        reg, val, strerror(errno));
    close(fd);
}

/* Deassert the external front-panel PHY reset (54282 1G + 84758 10G SFP+). ONL
 * leaves CPLD 0x19=0x7f/0x1b=0xb5 (PHYs HELD IN RESET, power-on default); ICOS
 * clears them. Without this the external MDIO is dead. */
/* Read one CPLD register (own fd). Returns -1 on error. */
static int
cpld_read(int reg)
{
    int fd = open("/dev/i2c-0", O_RDWR);
    int v;
    if (fd < 0) return -1;
    if (ioctl(fd, I2C_SLAVE_FORCE, 0x30) < 0) { close(fd); return -1; }
    v = cpld_read_fd(fd, (uint8_t)reg);
    close(fd);
    return v;
}

static void
deassert_ext_phy_reset(void)
{
    int r19, r1b;
    cpld_write(0x07, 0x02); cpld_write(0x08, 0x02); cpld_write(0x0d, 0x01);
    cpld_write(0x19, 0x00); cpld_write(0x1b, 0x00);
    r19 = cpld_read(0x19); r1b = cpld_read(0x1b);
    if (r19 == 0x00 && r1b == 0x00) {
        LOG("ext-phy: deasserted CPLD reset (0x19=0x00 0x1b=0x00 verified)");
    } else {
        LOG("ext-phy: WARNING CPLD reset NOT cleared (0x19=0x%02x 0x1b=0x%02x) "
            "— external PHYs may stay held in reset", r19, r1b);
    }
}

static void
write_icos_miim_map(void)
{
    static const struct { uint32_t off, val; } regs[] = {
        { 0x004, 0x00010012 }, { 0x008, 0x30001000 }, { 0x010, 0x08210001 },
        { 0x058, 0x00F00000 }, { 0x060, 0xFFFFFFFE }, { 0x064, 0x1101FFFF },
        { 0x074, 0x09249000 }, { 0x078, 0x09249249 }, { 0x07c, 0x03249249 },
        { 0x080, 0x00092480 }, { 0x084, 0x00000002 },
        /* bus->address map (octal-PHY gaps; ICOS skips 0x09/0x0a etc.) */
        { 0x094, 0x04030201 }, { 0x098, 0x08070605 }, { 0x09c, 0x0E0D0C0B },
        { 0x0a0, 0x1211100F }, { 0x0a4, 0x18171615 }, { 0x0a8, 0x1C1B1A19 },
        { 0x0ac, 0x04030201 }, { 0x0b0, 0x08070605 }, { 0x0b4, 0x0E0D0C0B },
        { 0x0b8, 0x1211100F }, { 0x0bc, 0x18171615 },
    };
    volatile uint32_t *m = (volatile uint32_t *)_mmap(0x48011000, 0x1000);
    unsigned k;
    if (m == NULL) { LOG("ext-mdio: mmap 0x48011000 failed"); return; }
    for (k = 0; k < sizeof(regs) / sizeof(regs[0]); k++) {
        m[regs[k].off / 4] = regs[k].val;
    }
    LOG("ext-mdio: wrote %u ICOS CMIC MIIM bus-map regs @0x48011000", k);
}

/* CMIC MIIM scan using ICOS's CMIC_MIIM_PARAM encoding (RE'd from switchdrvr
 * soc_miim_read @0x184683c). ICOS reaches the EXTERNAL front-panel PHYs (54282
 * 1G / 84758 10G) over the CMIC MIIM, building PARAM from the phy_addr as:
 *   bits[4:0]=device, bits[6:5]=bus, bit7->PARAM bit26 (int/clause sel).
 * OpenMDK cdk_xgsm_miim does a flat phy_addr<<16, so to produce ICOS's PARAM we
 * pass cdk_addr = device | (bus<<6) | (bit7<<10). (e.g. xe0 iaddr 0xc1 -> 0x481,
 * ge0 iaddr 0x81 -> 0x401.) Sweep bus + device, C45, look for 0x600d PHYs. */
static void
scan_cmic_miim_icos(int unit)
{
    int hi, bus, dev;
    uint32_t id0, id1;

    uint32_t addr;
    (void)hi; (void)bus; (void)dev;
    LOG("CMIC MIIM C45 sweep (phy_addr 0x000-0x3ff, all bus/select bits):");
    for (addr = 0; addr <= 0x3ff; addr++) {
        id0 = id1 = 0;
        /* cdk auto-adds the clause-45 select bit because devad is present. */
        if (cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0x02, &id0) < 0) {
            continue;
        }
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0x03, &id1);
        if ((id0 == 0 || id0 == 0xffff) && (id1 == 0 || id1 == 0xffff)) {
            continue;
        }
        LOG("  phy_addr 0x%03x: C45 ID %04x:%04x%s", addr, id0, id1,
            (id0 == 0x600d && (id1 & ~0xf) == 0x86f0) ? "  <== BCM84758" :
            (id0 == 0x600d && (id1 & ~0xf) == 0x845b) ? "  <== BCM54282" :
            (id0 == 0x143) ? "  (warpcore)" : "");
    }
    LOG("CMIC MIIM C45 sweep done");
}

/* MDIO scan: walk external + internal MIIM buses, read PMA-PMD ID (devad 1,
 * regs 2/3), and report any responding PHY — used to locate the BCM84758
 * (0x600d:0x86f0) so we can fix the port->MDIO-address map. */
static void
scan_mdio(int unit)
{
    int kind, b, a;
    uint32_t id0, id1, base;
    const char *name;

    LOG("MDIO scan: PMA-PMD ID (devad1 reg2/3) over EBUS0-3 + IBUS0-3, addr 0-15");
    LOG("looking for BCM84758 = 0x600d:0x86f0 ...");
    for (kind = 0; kind < 2; kind++) {
        name = kind ? "IBUS" : "EBUS";
        for (b = 0; b <= 3; b++) {
            base = kind ? CDK_XGSM_MIIM_IBUS(b) : CDK_XGSM_MIIM_EBUS(b);
            for (a = 0; a < 16; a++) {
                uint32_t addr = base | a;
                id0 = id1 = 0;
                if (cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0x02, &id0) < 0) {
                    continue;
                }
                cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0x03, &id1);
                if ((id0 == 0 || id0 == 0xffff) &&
                    (id1 == 0 || id1 == 0xffff)) {
                    continue;  /* nothing there */
                }
                LOG("  %s%d addr 0x%02x (miim 0x%03x): ID %04x:%04x%s",
                    name, b, a, addr, id0, id1,
                    (id0 == 0x600d && (id1 & ~0xf) == 0x86f0)
                        ? "   <==== BCM84758" : "");
            }
        }
    }
    LOG("CMIC MIIM scan done");

    /* Replay ICOS's CMIC MIIM bus-map/select regs to enable external MDIO,
     * then scan the front-panel PHYs over the CMIC MIIM. */
    write_icos_miim_map();
    scan_cmic_miim_icos(unit);

    /* The mgmt-port PHY hangs off the iProc CCB MDIO master. Scan that too. */
    scan_iproc_mdio();

    LOG("MDIO scan done");
}

/* Per-front-port (ge0..47) external 54282 MDIO address, taken VERBATIM from the
 * live board's config.bcm.as4610-54t (port_phy_addr_geN). The octal 54282s leave
 * a 2-address gap between packages (and a 4-wide gap at the 24-port bank seam),
 * so this is NOT a simple formula — it's a lookup. Front-panel jack = geN+1. */
static const uint8_t ge_phy_addr[48] = {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,  /* ge0-7   -> jacks 1-8   */
    0x0b,0x0c,0x0d,0x0e,0x0f,0x10,0x11,0x12,  /* ge8-15  -> jacks 9-16  */
    0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,  /* ge16-23 -> jacks 17-24 */
    0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,  /* ge24-31 -> jacks 25-32 */
    0x2b,0x2c,0x2d,0x2e,0x2f,0x30,0x31,0x32,  /* ge32-39 -> jacks 33-40 */
    0x35,0x36,0x37,0x38,0x39,0x3a,0x3b,0x3c,  /* ge40-47 -> jacks 41-48 */
};

/* Read the copper link bit straight from each 54282 over the CMIC MIIM at its
 * config.bcm address (EBUS0) — bypasses edged's port->phy map so we can see which
 * front-panel jack is actually linked, independent of the logical-port mapping.
 * 54282 is clause-22: MII status reg 0x01 bit 2 = link (latched, read twice);
 * reg 0x11 (Aux Status Summary) bits 8-10 = speed. */
/* Reverse-map a responding MDIO (bus,addr) back to its front-panel jack. The
 * board puts ge0-23 (jacks 1-24) on EBUS0 at addr 0x01-0x1c, and ge24-47
 * (jacks 25-48) on EBUS1 at the SAME low addrs — config.bcm encodes that upper
 * bank as the 0x20 bit (so config 0x21-0x3c == EBUS1 addr 0x01-0x1c). Returns -1
 * if (bus,addr) isn't a known ge jack. */
static int
jack_for_addr(int bus, uint8_t a)
{
    uint8_t cfg = bus ? (uint8_t)(0x20 | a) : a;
    int g;
    for (g = 0; g < 48; g++) if (ge_phy_addr[g] == cfg) return g + 1;
    return -1;
}

/* True if a C22 PHY-ID (reg2:reg3) is a BCM54282 copper PHY (600d:845x). */
static int
is_54282(uint32_t id1, uint32_t id2)
{
    return id1 == 0x600d && (id2 & 0xfff0) == 0x8450;
}

/* Empirical copper PHY/link sweep: walk EBUS0 (and EBUS1 as a fallback) reading
 * the 54282 via clause-22 — PHY ID (reg2/3), MII status (reg1, link bit 2), and
 * Aux Status (reg0x11, speed). Reports every address that answers so we can see
 * the TRUE 54282 layout and which front-panel jack is actually linked, regardless
 * of edged's internal port->phy map. */
static void
scan_link(int unit)
{
    int bus, a, answered = 0, up_count = 0;
    LOG("copper link sweep (clause-22): EBUS0/EBUS1, addr 0x01-0x3f");
    LOG("  per-addr: PHYID reg2:reg3, MII-status reg1 (bit2=link), Aux reg0x11");
    for (bus = 0; bus <= 1; bus++) {
        for (a = 1; a <= 0x3f; a++) {
            uint32_t addr = (bus ? CDK_XGSM_MIIM_EBUS(1) : CDK_XGSM_MIIM_EBUS(0)) | a;
            uint32_t id1 = 0, id2 = 0, st = 0, aux = 0;
            int jack, up;
            const char *spd = "?";
            if (cdk_xgsm_miim_read(unit, addr, 0x02, &id1) < 0) continue;
            cdk_xgsm_miim_read(unit, addr, 0x03, &id2);
            if ((id1 == 0xffff || id1 == 0x0000) &&
                (id2 == 0xffff || id2 == 0x0000)) continue;  /* nothing here */
            answered++;
            cdk_xgsm_miim_read(unit, addr, 0x01, &st);       /* clear latch */
            cdk_xgsm_miim_read(unit, addr, 0x01, &st);
            cdk_xgsm_miim_read(unit, addr, 0x11, &aux);
            up = (st & 0x0004) ? 1 : 0;
            switch ((aux >> 8) & 0x7) {
                case 0x7: spd="1000F"; break; case 0x6: spd="1000H"; break;
                case 0x5: spd="100F";  break; case 0x3: spd="100H";  break;
                case 0x2: spd="10F";   break; case 0x1: spd="10H";   break;
            }
            jack = jack_for_addr(bus, (uint8_t)a);
            LOG("  EBUS%d 0x%02x  id=%04x:%04x  st=0x%04x %s  aux=0x%04x spd=%s  %s",
                bus, a, id1, id2, st, up ? "LINK-UP" : "down", aux, spd,
                jack > 0 ? "" : "(not a ge jack)");
            if (jack > 0) LOG("        ^ front-panel JACK %d (ge%d)%s",
                              jack, jack - 1, up ? "  *** LINKED ***" : "");
            if (up) up_count++;
        }
    }
    LOG("copper link sweep done: %d PHY(s) answered, %d link UP", answered, up_count);
}

/* Kick every 54282 copper PHY into autoneg and re-read link. reg0 (MII control)
 * = 0x1200 = autoneg-enable(0x1000) + restart-autoneg(0x0200), with power-down
 * (bit11), isolate (bit10) and loopback (bit14) all CLEAR — so this also wakes a
 * powered-down copper PHY. We hit the 54282s found by ID (600d:845b) on EBUS0+1,
 * wait for autoneg, then report which front-panel jacks come up. */
static uint32_t
miim_addr(int bus, uint8_t a)
{
    return (bus ? CDK_XGSM_MIIM_EBUS(1) : CDK_XGSM_MIIM_EBUS(0)) | a;
}

static void
copper_up(int unit)
{
    int bus, a, n_phy = 0, up = 0, i, poll;
    struct { int bus; uint8_t a; } phys[128];
    uint32_t r0a = 0, r0b = 0, rb = 0;

    LOG("copper bring-up: collect 54282s, test addressing, restart autoneg, poll link");
    for (bus = 0; bus <= 1; bus++) {
        for (a = 1; a <= 0x3f; a++) {
            uint32_t id1 = 0, id2 = 0;
            if (cdk_xgsm_miim_read(unit, miim_addr(bus, (uint8_t)a), 0x02, &id1) < 0) continue;
            cdk_xgsm_miim_read(unit, miim_addr(bus, (uint8_t)a), 0x03, &id2);
            if (!is_54282(id1, id2)) continue;
            if (n_phy < 128) { phys[n_phy].bus = bus; phys[n_phy].a = (uint8_t)a; n_phy++; }
        }
    }
    LOG("  found %d BCM54282 copper PHYs", n_phy);
    if (n_phy < 2) { LOG("  too few PHYs to test"); return; }

    /* ADDRESSING/ALIASING TEST: write 0x0800 (isolate) to PHY[0] only, read both
     * PHY[0] and PHY[1]'s reg0. If PHY[1] also changed, the MDIO addressing is
     * aliasing (not selecting individual PHYs) -> explains identical status. */
    cdk_xgsm_miim_write(unit, miim_addr(phys[0].bus, phys[0].a), 0x00, 0x0800);
    cdk_xgsm_miim_write(unit, miim_addr(phys[1].bus, phys[1].a), 0x00, 0x1000);
    cdk_xgsm_miim_read(unit, miim_addr(phys[0].bus, phys[0].a), 0x00, &r0a);
    cdk_xgsm_miim_read(unit, miim_addr(phys[1].bus, phys[1].a), 0x00, &r0b);
    LOG("  addr-test: PHY0(EBUS%d 0x%02x) reg0<-0x0800 reads 0x%04x ; "
        "PHY1(EBUS%d 0x%02x) reg0<-0x1000 reads 0x%04x  => %s",
        phys[0].bus, phys[0].a, r0a, phys[1].bus, phys[1].a, r0b,
        (r0a != r0b) ? "DISTINCT (addressing OK)" : "SAME (ALIASED!)");

    /* Restart autoneg on every copper PHY (0x1200 = AN enable+restart, power-up,
     * no isolate/loopback), confirm the write sticks on PHY0. */
    for (i = 0; i < n_phy; i++)
        cdk_xgsm_miim_write(unit, miim_addr(phys[i].bus, phys[i].a), 0x00, 0x1200);
    cdk_xgsm_miim_read(unit, miim_addr(phys[0].bus, phys[0].a), 0x00, &rb);
    LOG("  reg0 readback after AN-restart on PHY0: 0x%04x (expect 0x1000/0x1200)", rb);

    /* Poll link for up to 8s. */
    for (poll = 0; poll < 8; poll++) {
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
        up = 0;
        for (i = 0; i < n_phy; i++) {
            uint32_t st = 0;
            cdk_xgsm_miim_read(unit, miim_addr(phys[i].bus, phys[i].a), 0x01, &st);
            cdk_xgsm_miim_read(unit, miim_addr(phys[i].bus, phys[i].a), 0x01, &st);
            if (st & 0x0004) up++;
        }
        LOG("  t=%ds: %d/%d copper PHY(s) link UP", poll + 1, up, n_phy);
        if (up > 0) break;
    }

    /* DECISIVE: does ANY copper port see the cable? Read Broadcom real-time
     * status regs for all 48 and flag any that differ from PHY0 — a cabled port
     * (energy / AN-in-progress / link) will diverge from the idle pack.
     *   reg 0x19 = Aux Status Summary (b2=link, b8-10=speed, b15-13 misc)
     *   reg 0x0a = 1000BASE-T Status  (b13=LP 1000T-able, b11/12 rx-status)
     *   reg 0x18.shadow / reg 0x01 already known idle. */
    {
        uint32_t a19_0 = 0, a0a_0 = 0, st_0 = 0;
        int diff = 0;
        cdk_xgsm_miim_read(unit, miim_addr(phys[0].bus, phys[0].a), 0x19, &a19_0);
        cdk_xgsm_miim_read(unit, miim_addr(phys[0].bus, phys[0].a), 0x0a, &a0a_0);
        cdk_xgsm_miim_read(unit, miim_addr(phys[0].bus, phys[0].a), 0x01, &st_0);
        LOG("  baseline (PHY0): reg1=0x%04x reg0x19=0x%04x reg0x0a=0x%04x", st_0, a19_0, a0a_0);
        for (i = 0; i < n_phy; i++) {
            uint32_t a19 = 0, a0a = 0, st = 0;
            int jack;
            cdk_xgsm_miim_read(unit, miim_addr(phys[i].bus, phys[i].a), 0x19, &a19);
            cdk_xgsm_miim_read(unit, miim_addr(phys[i].bus, phys[i].a), 0x0a, &a0a);
            cdk_xgsm_miim_read(unit, miim_addr(phys[i].bus, phys[i].a), 0x01, &st);
            cdk_xgsm_miim_read(unit, miim_addr(phys[i].bus, phys[i].a), 0x01, &st);
            if (a19 == a19_0 && a0a == a0a_0 && (st & 0x0004) == 0) continue; /* same as idle */
            jack = jack_for_addr(phys[i].bus, phys[i].a);
            LOG("  DIFFERS: EBUS%d 0x%02x (jack %d): reg1=0x%04x reg0x19=0x%04x reg0x0a=0x%04x%s",
                phys[i].bus, phys[i].a, jack, st, a19, a0a, (st & 0x0004) ? "  LINK!" : "");
            diff++;
        }
        if (!diff)
            LOG("  ALL 48 copper PHYs read IDENTICAL idle status — NO port sees a "
                "cable/energy. Copper side is uniformly un-driven (line-side/init), "
                "not a per-jack issue.");
    }
    LOG("copper bring-up done: %d/%d PHY(s) link UP", up, n_phy);
}

/* Enable the SFP+ optical transmitter on the BCM84758 (xe0-3 at EBUS2 0x80-0x83).
 * OpenMDK's bcm84740 driver never deasserts TX — so the 84758 holds the SFP
 * TX_DISABLE pin asserted (laser off -> RX_LOS at the link partner). Replicates
 * robo2-xsdk phy_84740_enable_set(enable=1): clear the IEEE PMD global TX-disable
 * (1.0009 bit0) + the 84740 GENSIG TX-disable (1.0x8071 bit3). */
/* BCM84740-family optical-interface registers (PMA/PMD = devad 1). Addresses +
 * masks are VERBATIM from the robo2-xsdk phy84740.h (Broadcom source-available):
 *   1.0xc8e4 OPTICAL_CFG : b3=MOD_PRESENCE, b4=lane power(0=on), b12=TxOn(0=on),
 *                          RXLOS_OVERRIDE=0xc0c0, MOD_ABS_OVERRIDE=0x0808.
 *   1.0xc800 OPTICAL_SIG_LVL : RXLOS_LVL=b9, MOD_ABS_LVL=b8, TxOnOff strap=b7.
 *   1.0x0009 PMD_TX_DISABLE  : b0=global PMD TX disable (0=on).
 *   1.0xcd16 GENSIG_8071     : b3=TX disable in LP/single mode (0=on).
 *   1.0xc702 AER_ADDR        : lane select (0 for these single-port SFP+). */
#define X84_OPT_CFG     0xc8e4
#define X84_OPT_SIGLVL  0xc800
#define X84_TX_DISABLE  0x0009
#define X84_GENSIG_8071 0xcd16
#define X84_AER_ADDR    0xc702
#define X84_PMAD_CTRL   0x0000   /* PMA/PMD control 1: speed-select bits */
#define X84_PMAD_CTRL2  0x0007   /* PMA/PMD control 2: PMA type (low nibble) */
#define X84_SS_10G      0x2040   /* MII_CTRL_SS_MSB(b6) | SS_LSB(b13) = 10G */
#define X84_PMA_TYPE_10G_LRM 0x8 /* PMA type field = 10GBASE-LRM */
#define X84_RESET_CTRL  0xcd17   /* RESET_CONTROL_REGISTER: clear to enable the PCS */
#define X84_RXLOS_OVERRIDE 0xc0c0
#define X84_MODABS_OVERRIDE 0x0808
#define X84_RXLOS_LVL   (1u << 9)
#define X84_MODABS_LVL  (1u << 8)
#define X84_MOD_PRESENCE (1u << 3)
#define X84_SIDE_SEL    0xffff   /* 1.0xffff b0: 0=MMF/line(optics), 1=XFI/system */
#define X84_CHIP_MODE   0xc805   /* PMAD_CHIP_MODE: b3=DAC, b2=backplane (clear both=optical SR/LR) */
#define X84_DAC_MODE    (1u << 3)
#define X84_BKPLANE_MODE (1u << 2)
#define X84_REPEATER_DET 0xc81d  /* (b1|b2)=0x6 => repeater/retimer mode */
#define X84_SQUELCH_CTL 0xcd18   /* devad 3 (PCS): RX squelch enable per speed */
#define X84_EDC_MODE    0xca1a   /* EDC mode (media-RX adaptation); SR/LR optics = 0x44 */
#define X84_EDC_SR_LR   0x44
#define X84_EDC_MASK    0xff

/* Read `count` bytes from i2c device `dev` (SFP EEPROM = 0x50) at `offset` via the
 * BCM84758's OWN 2-wire/BSC master (port s -> EBUS2|s, lane 0). Ports the robo2
 * _phy_84740_bsc_rw READ path (non-single-port branch): stage RAM/addr/count/dev,
 * issue READ_OP at 1.0x8000, poll 2W_STAT(b3:2)==COMPLETE(0x4), read bytes from
 * 1.0x8007+i. Returns 0 ok, -1 timeout, -2 fail. Proves whether the 84758's i2c
 * is physically wired to the SFP module (vs the CPU i2c-mux/optoe path). */
static int
sfp_i2c_read_via_phy(int unit, int s, int dev, int offset, int count, uint8_t *buf)
{
    uint32_t addr = CDK_XGSM_MIIM_EBUS(2) | s;
    uint32_t v = 0;
    int i, to;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };  /* 10ms */
    cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_AER_ADDR, 0);   /* lane 0 */
    cdk_xgsm_miim_write(unit, addr, (1 << 16) | 0x8004, 0x8007);    /* RAM start */
    cdk_xgsm_miim_write(unit, addr, (1 << 16) | 0x8003, offset);
    cdk_xgsm_miim_write(unit, addr, (1 << 16) | 0x8002, count);
    cdk_xgsm_miim_write(unit, addr, (1 << 16) | 0x8005, 1 | (dev << 9));
    cdk_xgsm_miim_write(unit, addr, (1 << 16) | 0x8000, 0x8000 | 0x2); /* BSC READ_OP */
    for (to = 0; to < 100; to++) {
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0x8000, &v);
        if ((v & 0x000c) == 0x0004) break;    /* COMPLETE */
        if ((v & 0x000c) == 0x000c) return -2; /* FAIL */
        nanosleep(&ts, NULL);
    }
    if ((v & 0x000c) != 0x0004) return -1;    /* timeout/IN_PRG */
    nanosleep(&ts, NULL);
    for (i = 0; i < count; i++) {
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | (0x8007 + i), &v);
        buf[i] = (uint8_t)(v & 0xff);
    }
    return 0;
}

/* Port the robo2-xsdk phy84740 optical bring-up + enable_set for each SFP+ (xe0-3
 * at EBUS2 0x80-0x83). OpenMDK's bcm84740 driver sets the c8e4 override but NEVER
 * the c800 RXLOS/MOD_ABS *level* bits — so the overridden RX_LOS resolves to "LOS"
 * and the media PCS pins sigdet=0. The full robo2 sequence: (1) c8e4 override +
 * MOD_PRESENCE, (2) c800 set RXLOS_LVL|MOD_ABS_LVL so the override means "signal
 * present / module present", (3) clear PMD/GENSIG TX-disable, (4) TxOnOff strap:
 * c8e4[4] = strap XOR c800[7], toggle c800[7] to make c8e4[4]=0 (laser on). */
static void
sfp_tx_enable(int unit)
{
    int s;
    LOG("SFP+ 84758: robo2 optical bring-up (c8e4 override + c800 levels b8/b9 + "
        "TX-enable) on xe0-3");
    for (s = 0; s < 4; s++) {
        uint32_t addr = CDK_XGSM_MIIM_EBUS(2) | s;
        uint32_t v = 0;
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_AER_ADDR, 0);  /* lane 0 */

        /* (0-pre-a) Force MMF/LINE side: 1.0xffff b0 = 0. The 84758 side-latch is
         * stateful; if it was left at XFI(=1) our media (1.0007/c8e4/c800/cd17)
         * writes would land on the SYSTEM side and the optical PMD never configures
         * -> sd=0. (robo2 PHY84740_MMF.) */
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_SIDE_SEL, &v);
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_SIDE_SEL, v & ~0x1u);

        /* (0-pre-b) Select the OPTICAL front-end: clear DAC (b3) + backplane (b2)
         * in 1.0xc805 (PMAD_CHIP_MODE). robo2 phy_84740_init does this for SR/LR;
         * if the PMD is left in DAC/copper mode it never locks on optical light, so
         * signal-detect stays 0 with the fiber lit. This is the fiber-media select
         * that the SDK's phy_fiber_pref=1 ultimately produces. */
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_CHIP_MODE, &v);
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_CHIP_MODE,
                            v & ~(X84_DAC_MODE | X84_BKPLANE_MODE));

        /* (0-pre-c) DISABLE AN (7.0) + CL72 link-training (1.0x0096). 10GBASE-LR
         * optical has NO autoneg / no CL72 link-training; if either is left armed on
         * the 84758, its media RX waits forever on a negotiation the LR peer never
         * runs -> signal-detect (1.000a) never asserts (sd=0). robo2 phy_84740_an_set
         * / init disable these (phy84740.c:1517-1521). edged never did -> prime suspect. */
        { uint32_t an=0, cl72=0;
          cdk_xgsm_miim_read(unit, addr, (7 << 16) | 0x0000, &an);
          cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0x0096, &cl72);
          if (s == 0) LOG("    xe0 before AN(7.0)=%04x CL72(1.0096)=%04x -> disabling",
                          an & 0xffff, cl72 & 0xffff);
          cdk_xgsm_miim_write(unit, addr, (7 << 16) | 0x0000, an & ~(1u << 12)); /* AN_EN off */
          cdk_xgsm_miim_write(unit, addr, (1 << 16) | 0x0096, 0x0000);           /* CL72 off */
        }

        /* (0) Set the 84758 PMA to 10G (robo2 phy_84740_speed_set). OpenMDK loads
         * the ucode but NEVER configures the speed, so the media datapath never
         * runs and the PMD signal-detect stays 0. CTRL1 = speed-select MSB|LSB
         * (0x2040 = 10G); CTRL2 PMA-type field (low nibble) = 10GBASE-LRM (0x8). */
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_PMAD_CTRL, X84_SS_10G);
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_PMAD_CTRL2, &v);
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_PMAD_CTRL2,
                            (v & ~0xfu) | X84_PMA_TYPE_10G_LRM);

        /* (0b) ENABLE THE PCS: clear 1.0xcd17 (RESET_CONTROL_REGISTER). In multi-
         * port (4x10G, single-lane) mode the 84758 holds the media PCS in reset
         * until this is cleared — the step OpenMDK omits that pins the media PMD
         * off (sd=0) and stops 10GBASE-R block-lock. (robo2 phy_84740_init.) */
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_RESET_CTRL, 0);

        /* (1) OPTICAL_CFG = the exact ICOS LOCKED-state value 0xc8ef (RXLOS+MOD_ABS
         * override on, MOD_PRESENCE, lane-power on b4=0, TxOn b12=0). Captured from
         * a locked ICOS 84758 (2026-06-06, dumps/icos_linked_2026_06_06/21_*). */
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_OPT_CFG, 0xc8ef);

        /* (2) OPTICAL_SIG_LVL = the exact ICOS LOCKED value 0x3f3f. THE FIX: edged
         * previously set only RXLOS_LVL(b9)+MOD_ABS_LVL(b8) = 0x300, but ICOS also
         * programs the full RX_LOS/signal-level THRESHOLD fields (b0-5, b10-13).
         * Without those thresholds the media RX never declares signal-detect, so
         * 1.000a sd stayed 0 and the 10GBASE-R PCS never block-locked. With 0x3f3f a
         * locked ICOS reads 1.000a=1 / 3.0020 block-lock=1. */
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_OPT_SIGLVL, 0x3f3f);

        /* (2b) c82b = ICOS locked value 0x8a40 (edged previously left this default). */
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | 0xc82b, 0x8a40);

        /* NOTE: a full ICOS-vs-edged register diff (2026-06-06) showed only 16 regs
         * differ, ALL uC-managed RX-DSP/adaptation outputs or status (c804/c81f/c840/
         * c842/c873-c87a/cd04/cd09/cd3d/c820/c848/c878). Forcing edged to write all of
         * them to the ICOS values did NOT lock the media RX -> the lock is the dynamic
         * uC adaptation PROCESS, not any static register value. So we do NOT write them
         * (they're firmware outputs). The static CONFIG (c800/c8e4/c82b/c805/c701/cd17)
         * already matches ICOS. */

        /* (3) Clear PMD global TX-disable (1.0009 b0) + GENSIG TX-disable (1.cd16 b3). */
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_TX_DISABLE, &v);
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_TX_DISABLE, v & ~0x1u);
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_GENSIG_8071, &v);
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_GENSIG_8071, v & ~0x8u);

        /* Ensure TxOn (c8e4 b12) + lane power (c8e4 b4 — but it's strap-driven). */
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_OPT_CFG, &v);
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_OPT_CFG, v & ~(1u << 12));

        /* (4) TxOnOff strap: c8e4[4] = TxOnOff_pin XOR c800[7]. If c8e4[4]=1 (laser
         * low-power/off), toggle c800[7] to flip it to 0 (TX active). */
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_OPT_CFG, &v);
        if (v & (1u << 4)) {
            uint32_t c800 = 0;
            cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_OPT_SIGLVL, &c800);
            cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_OPT_SIGLVL,
                                (c800 & (1u << 7)) ? (c800 & ~(1u << 7))
                                                   : (c800 | (1u << 7)));
        }

        /* (5) Squelch-enable: if the 84758 is in repeater/retimer mode
         * (1.0xc81d b1|b2 = 0x6), un-squelch the media->system datapath via the
         * SYSTEM-side PCS reg 3.0xcd18 (10G => bit4 RX + bit6 enable = 0x50), so the
         * locked media RX is repeated to the Warpcore. robo2 _phy_84740_squelch_enable.
         * (Without this, even a locked media side won't reach the Warpcore.) */
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_REPEATER_DET, &v);
        if ((v & 0x6) == 0x6) {
            uint32_t sq = 0;
            cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_SIDE_SEL, 1); /* XFI/system */
            cdk_xgsm_miim_read(unit, addr, (3 << 16) | X84_SQUELCH_CTL, &sq);
            cdk_xgsm_miim_write(unit, addr, (3 << 16) | X84_SQUELCH_CTL,
                                sq | (1u << 4) | (1u << 6));
            cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_SIDE_SEL, 0); /* back to MMF */
        }
    }
}

/* Warpcore UC_CTRL (0x820e) fw-pause/host-cal RE-ARM, ported from the full XGS SDK
 * _phy_wc40_firmware_mode_set (sdk-xgs-robo-6.4.1 wc40.c:724-808) + the AS5610 decode
 * (docs/WC_FIRMWARE_PAUSE_PROTOCOL.md). OpenMDK's bcmi_warpcore_xgxs sets FIRMWARE_MODE
 * but NEVER performs the STOP->set-mode->RESUME->RESTART uC handshake, so the Warpcore
 * RX DSC/EQ is never re-armed for the XFI link: it coarse-locks (PCS RXlink=1) but the
 * EQ doesn't converge, and the 84758 media side (seeing a marginal WC<->84758 link)
 * never adapts (sd=0). This re-arms it, per SFP+ Warpcore lane.
 *   UC_CTRL 0x5000820e: [15:8]=SUPPLEMENT_INFO(cmd 0=stop,2=resume,3=restart),
 *                       b7=READY_FOR_CMD, [3:0]=GP_UC_REQ(=1 to issue).
 *   FIRMWARE_MODE 0x501081f2: LN<lane>_MODE = 4 bits at (4*lane); 0=DEFAULT (XFI). */
#define WC_UC_CTRL_REG  0x5000820e
#define WC_FW_MODE_REG  0x501081f2
#define WC_UC_READY     (1u << 7)

static int
wc_uc_wait_ready(phy_ctrl_t *wc)
{
    int i;
    uint32_t v = 0;
    struct timespec ms = { .tv_sec = 0, .tv_nsec = 1000000 };  /* 1ms */
    for (i = 0; i < 300; i++) {
        phy_aer_iblk_read(wc, WC_UC_CTRL_REG, &v);
        if (v & WC_UC_READY) return 0;
        nanosleep(&ms, NULL);
    }
    return -1;
}

static void
wc_uc_cmd(phy_ctrl_t *wc, uint32_t supplement)
{
    uint32_t v = 0;
    struct timespec ms = { .tv_sec = 0, .tv_nsec = 1000000 };  /* 1ms */
    phy_aer_iblk_read(wc, WC_UC_CTRL_REG, &v);
    v = (v & ~0xff0fu) | ((supplement & 0xff) << 8) | 0x1;     /* SUPPLEMENT_INFO + GP_UC_REQ=1 */
    phy_aer_iblk_write(wc, WC_UC_CTRL_REG, v);
    nanosleep(&ms, NULL);
    wc_uc_wait_ready(wc);
}

/* Re-arm the Warpcore RX adaptation for one SFP+ port via the uC fw-pause handshake. */
static int
warpcore_fw_rearm(int unit, int port)
{
    phy_ctrl_t *pc, *wc = NULL;
    uint32_t v = 0;
    int lane;
    for (pc = BMD_PORT_PHY_CTRL(unit, port); pc != NULL; pc = pc->next) {
        if (pc->drv && pc->drv->drv_name &&
            !strcmp(pc->drv->drv_name, "bcmi_warpcore_xgxs")) { wc = pc; break; }
    }
    if (!wc) return -1;
    lane = PHY_CTRL_INST(wc) & 0x3;
    if (wc_uc_wait_ready(wc) < 0) {
        LOG("  port %d: Warpcore uC not ready (pre) — skip re-arm", port);
        return -1;
    }
    wc_uc_cmd(wc, 0);                                          /* STOP */
    phy_aer_iblk_read(wc, WC_FW_MODE_REG, &v);                 /* set FW mode = DEFAULT (XFI) */
    v &= ~(0xfu << (4 * lane));
    phy_aer_iblk_write(wc, WC_FW_MODE_REG, v);
    wc_uc_cmd(wc, 2);                                          /* RESUME */
    wc_uc_cmd(wc, 3);                                          /* RESTART */
    LOG("  port %d: Warpcore fw-pause re-arm (lane %d) done", port, lane);
    return 0;
}

/* 84758 media-RX RE-ACQUIRE via the EDC-mode programming sequence, ported from robo2
 * _phy_84740_control_edc_mode_set (phy84740.c:3310). THE 84758-side analog of the
 * Warpcore handshake: edged sets c800 bit9 (RXLOS_LVL = "signal present") STATICALLY
 * from the start, so the 84758 uC never sees the LOS->present EDGE that triggers media-
 * RX acquisition -> sd stays 0. This sequence: clear the c8e4 RX_LOS/MOD_ABS overrides,
 * INDUCE a fake LOS (flip c800 b9), (re)program the EDC mode (0xca1a = 0x44 SR/LR),
 * REMOVE the LOS (restore c800 b9), restore overrides -- forcing the uC to re-acquire
 * the media RX. Per SFP+ port (quad-port, single lane). */
static void
sfp_edc_reacquire(int unit)
{
    int s;
    struct timespec ms = { .tv_sec = 0, .tv_nsec = 5 * 1000 * 1000 };  /* 5ms */
    LOG("SFP+ 84758 media-RX re-acquire (EDC LOS-toggle, robo2 edc_mode_set):");
    for (s = 0; s < 4; s++) {
        uint32_t addr = CDK_XGSM_MIIM_EBUS(2) | s;
        uint32_t v = 0, c800 = 0;
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_AER_ADDR, 0);   /* lane 0 */
        /* 1. clear RX_LOS/MOD_ABS overrides (real signalling during the sequence) */
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_OPT_CFG, &v);
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_OPT_CFG,
                            v & ~(X84_RXLOS_OVERRIDE | X84_MODABS_OVERRIDE));
        /* 2. induce LOS: flip c800 bit9 (RXLOS_LVL) to its inverse */
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_OPT_SIGLVL, &c800);
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_OPT_SIGLVL,
                            (c800 & ~X84_RXLOS_LVL) | ((~c800) & X84_RXLOS_LVL));
        nanosleep(&ms, NULL);
        /* 3. (re)program the EDC mode = SR/LR optics (0x44) */
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_EDC_MODE, &v);
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_EDC_MODE,
                            (v & ~X84_EDC_MASK) | X84_EDC_SR_LR);
        nanosleep(&ms, NULL);
        /* 4. remove LOS: restore c800 bit9 to its original value (signal "returns") */
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_OPT_SIGLVL, &v);
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_OPT_SIGLVL,
                            (v & ~X84_RXLOS_LVL) | (c800 & X84_RXLOS_LVL));
        /* 5. restore the RX_LOS/MOD_ABS overrides */
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_OPT_CFG, &v);
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_OPT_CFG,
                            v | X84_RXLOS_OVERRIDE | X84_MODABS_OVERRIDE);
    }
}

/* The 84758 8051 microcode, linked in via the OpenMDK patch (bcm84758_ucode.c). */
extern unsigned char bcm84758_ucode[];
extern unsigned int  bcm84758_ucode_len;

/* Re-download the 84758 ucode over the MDIO mailbox (robo2 _phy84740_mdio_firmware_
 * download, quad-port path): prep SPA_CTRL(0xc848)+MISC_CTRL1(0xca85), MII-reset to
 * start the uC bootloader, then feed the image word-by-word through M8051_MSGIN
 * (1.0xca12); the bootloader writes the checksum to 1.0xca1c (expect 0x600d).
 * Returns the checksum. */
static uint32_t
sfp_84758_dl_ucode(int unit, uint32_t addr)
{
    unsigned int j, n = bcm84758_ucode_len;
    uint32_t v = 0;
    struct timespec ms10 = { .tv_sec = 0, .tv_nsec = 10*1000*1000 };
    struct timespec us200 = { .tv_sec = 0, .tv_nsec = 200*1000 };
    /* SPA_CTRL: clear b15(SPI-ROM-dl) + b13(dl-done), set b14(RAM boot) -> MDIO-to-RAM */
    cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0xc848, &v);
    cdk_xgsm_miim_write(unit, addr, (1 << 16) | 0xc848,
                        (v & ~((1u << 15) | (1u << 13))) | (1u << 14));
    /* MISC_CTRL1 b3 = 1: 32K download size */
    cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0xca85, &v);
    cdk_xgsm_miim_write(unit, addr, (1 << 16) | 0xca85, v | (1u << 3));
    /* MII reset -> start M8051 bootloader */
    cdk_xgsm_miim_write(unit, addr, (1 << 16) | 0x0000, 0x8000);
    nanosleep(&ms10, NULL);
    /* MSGIN: SRAM start address, word count, then the image words */
    cdk_xgsm_miim_write(unit, addr, (1 << 16) | 0xca12, 0x8000);
    cdk_xgsm_miim_write(unit, addr, (1 << 16) | 0xca12, n / 2);
    for (j = 0; j + 1 < n; j += 2) {
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | 0xca12,
                            ((uint32_t)bcm84758_ucode[j] << 8) | bcm84758_ucode[j + 1]);
    }
    nanosleep(&us200, NULL);
    cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0xca13, &v);   /* MSGOUT done handshake */
    cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0x9003, &v);   /* clear LASI */
    nanosleep(&us200, NULL);
    cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0xca13, &v);
    cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0xca1c, &v);   /* checksum */
    return v & 0xffff;
}

/* WHOLESALE 84758 re-init in robo2's complete order (the last 10G lead): fresh PMA
 * soft-reset -> re-download ucode -> apply config in order -> enable. The theory:
 * edged's piecemeal config on top of OpenMDK's partial init left the media-RX uC in
 * a state where it never acquires; a full fresh init may fix it. Per SFP+ port (quad). */
static void
sfp_full_reinit(int unit)
{
    int s;
    struct timespec ms12 = { .tv_sec = 0, .tv_nsec = 12*1000*1000 };
    LOG("SFP+ 84758 WHOLESALE re-init (chip-mode->L2P->reset+re-ucode->config->enable):");
    for (s = 0; s < 4; s++) {
        uint32_t addr = CDK_XGSM_MIIM_EBUS(2) | s, v = 0, cs;
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_AER_ADDR, 0);
        /* MMF/line side + optical chip-mode + L2P map (BEFORE the reset, robo2 order) */
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_SIDE_SEL, &v);
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_SIDE_SEL, v & ~0x1u);
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_CHIP_MODE, &v);
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_CHIP_MODE,
                            v & ~(X84_DAC_MODE | X84_BKPLANE_MODE));
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | 0xc701, 0x3210);   /* L2P lane map */
        /* fresh reset + re-download ucode */
        cs = sfp_84758_dl_ucode(unit, addr);
        if (s == 0) LOG("    xe0 re-ucode checksum = 0x%04x (%s)", cs, cs == 0x600d ? "OK" : "FAIL");
        nanosleep(&ms12, NULL);
        /* PCS enable */
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_RESET_CTRL, 0);
        /* optical overrides + levels (robo2 init values: c8e4 override, c800 b8/b9) */
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_OPT_CFG, &v);
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_OPT_CFG,
                            v | X84_RXLOS_OVERRIDE | X84_MODABS_OVERRIDE | X84_MOD_PRESENCE);
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_OPT_SIGLVL, &v);
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_OPT_SIGLVL,
                            v | X84_RXLOS_LVL | X84_MODABS_LVL);
        /* speed 10G + PMA type 10G-LRM (media side) */
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_PMAD_CTRL, X84_SS_10G);
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_PMAD_CTRL2, &v);
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_PMAD_CTRL2,
                            (v & ~0xfu) | X84_PMA_TYPE_10G_LRM);
        /* enable TX/laser (clear TX-disable, TxOn b12, lane power b4, c800[7] strap) */
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_TX_DISABLE, &v);
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_TX_DISABLE, v & ~0x1u);
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_OPT_CFG, &v);
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_OPT_CFG, v & ~(1u << 12));
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_OPT_CFG, &v);
        if (v & (1u << 4)) {
            uint32_t c8 = 0;
            cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_OPT_SIGLVL, &c8);
            cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_OPT_SIGLVL,
                                (c8 & (1u << 7)) ? (c8 & ~(1u << 7)) : (c8 | (1u << 7)));
        }
        cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_OPT_CFG, &v);
        cdk_xgsm_miim_write(unit, addr, (1 << 16) | X84_OPT_CFG, v & ~(1u << 4));
    }
}

/* Direct optical-link read for an SFP+ port (logical SFP_LO..SFP_HI -> 84758 at
 * EBUS2|slot, slot = port - SFP_LO). OpenMDK's bmd_phy_link_get walks the cascaded
 * chain and returns 0 for these ports (the 84740 latched-status read + Warpcore
 * link_get both report 0 even when the optics are locked), so it can't drive the
 * MAC up. We instead read the 84758 media side directly the same way --up-check
 * does (and that read agrees byte-for-byte with a locked ICOS): PMD STAT1 (1.0001)
 * link-alive bit2 AND PCS STAT1 (3.0001) link-alive bit2, with the latch-low double
 * read, backed by the 10GBASE-R block-lock (3.0020 bit0). Returns 1 = optics up. */
static int
sfp_link_alive(int unit, int port)
{
    uint32_t addr = CDK_XGSM_MIIM_EBUS(2) | (uint32_t)(port - SFP_LO);
    uint32_t pmd_st = 0, pcs_st = 0, blk = 0;

    /* clear latched local-fault first (reg 8), then read STAT1 twice (bit2 is
     * latch-low so the 1st read clears the latch, the 2nd reflects current). */
    cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0x0008, &pmd_st);
    cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0x0001, &pmd_st);
    cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0x0001, &pmd_st);
    cdk_xgsm_miim_read(unit, addr, (3 << 16) | 0x0001, &pcs_st);
    cdk_xgsm_miim_read(unit, addr, (3 << 16) | 0x0001, &pcs_st);
    cdk_xgsm_miim_read(unit, addr, (3 << 16) | 0x0020, &blk);   /* 10GBASE-R block-lock b0 */

    /* Link up = media PMD sees light (PMD LA) AND the 10GBASE-R PCS is locked
     * (PCS LA or block-lock). block-lock is the steadier indicator once locked. */
    return ((pmd_st & 0x4) && ((pcs_st & 0x4) || (blk & 0x1))) ? 1 : 0;
}

static int
parse_unit(const char *s, uint32_t *ven, uint32_t *dev, uint32_t *rev, uint32_t *base)
{
    return sscanf(s, "0x%x:0x%x:0x%x@0x%x", ven, dev, rev, base) == 4 ? 0 : -1;
}

static void
usage(const char *p)
{
    fprintf(stderr,
        "usage: %s [--unit V:D:R@BASE] [--keep] [--l3 FILE] [--l3-show]\n"
        "  --unit     device spec (default: %s)\n"
        "  --keep     stay resident: monitor link + re-sync MAC on link changes\n"
        "  --l3 FILE  program IPv4 L3 forwarding from config FILE\n"
        "  --l3-show  read back + print the programmed L3 tables\n",
        p, EDGED_DEFAULT_UNIT);
}

/* Resident control loop: poll link, log transitions, re-sync MAC on change. */
static void
control_loop(int unit)
{
    int i;

    LOG("--keep: control loop active (%ds poll, %d ports)", LINK_POLL_SEC, n_front);
    for (;;) {
        for (i = 0; i < n_front; i++) {
            int port = front_ports[i];
            bmd_port_mode_t mode;
            uint32_t flags = 0;
            int up;

            /* SFP+ ports: bmd_phy_link_get can't see the locked optics (returns 0
             * down the cascaded 84740/Warpcore chain), so drive the BMD link state
             * ourselves from the direct 84758 media read. BMD_PST_FORCE_LINK makes
             * bmd_link_update skip the (broken) PHY read and trust BMD_PST_LINK_UP;
             * bmd_port_mode_update then deasserts the XMAC soft-reset and sets the
             * EPC_LINK_BMAP egress bit on the up-edge (and reverses both on down). */
            if (port >= SFP_LO && port <= SFP_HI) {
                if (sfp_link_alive(unit, port)) {
                    BMD_PORT_STATUS_SET(unit, port,
                                        BMD_PST_FORCE_LINK | BMD_PST_LINK_UP);
                } else {
                    BMD_PORT_STATUS_SET(unit, port, BMD_PST_FORCE_LINK);
                    BMD_PORT_STATUS_CLR(unit, port, BMD_PST_LINK_UP);
                }
            }

            /* Let the BMD reconcile MAC config with current link status. */
            bmd_port_mode_update(unit, port);

            if (bmd_port_mode_get(unit, port, &mode, &flags) < 0) {
                continue;
            }
            up = (flags & BMD_PORT_MODE_F_LINK_UP) ? 1 : 0;
            if (up != link_state[i]) {
                LOG("port %d link %s%s%s", port,
                    up ? "UP" : "DOWN",
                    up ? " @ " : "",
                    up ? mode_str(mode) : "");
                link_state[i] = up;
            }
        }
        {
            struct timespec ts = { .tv_sec = LINK_POLL_SEC, .tv_nsec = 0 };
            nanosleep(&ts, NULL);
        }
    }
}

int
main(int argc, char *argv[])
{
    const char *unitspec = EDGED_DEFAULT_UNIT;
    uint32_t ven, dev, rev, base;
    int keep = 0;
    int scan = 0;
    int try_10g = 0;   /* --try-10g: experimental SFP+ 10G dyn-config (breaks L2) */
    const char *l3_conf = NULL;  /* --l3 <file>: program IPv4 L3 from a config */
    int l3_show_flag = 0;        /* --l3-show: read back L3 tables after programming */
    int unit = 0;
    int rc, i, port;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--unit") || !strcmp(argv[i], "-u")) {
            if (++i >= argc) { usage(argv[0]); return 2; }
            unitspec = argv[i];
        } else if (!strcmp(argv[i], "--memdump")) {
            /* --memdump <hexaddr> <nwords>: raw /dev/mem region dump, then exit.
             * Used to capture/diff iProc + CMIC register state vs ICOS. */
            uint32_t a, nw, j, pg, off;
            volatile uint32_t *m;
            if (i + 2 >= argc) { usage(argv[0]); return 2; }
            a = (uint32_t)strtoul(argv[i + 1], NULL, 16);
            nw = (uint32_t)strtoul(argv[i + 2], NULL, 0);
            pg = a & ~0xfffu; off = a - pg;
            m = _mmap(pg, (off + nw * 4 + 0xfff) & ~0xfffu);
            if (m == NULL) { return 1; }
            for (j = 0; j < nw; j++) {
                printf("0x%08x: 0x%08x\n", a + j * 4, m[(off / 4) + j]);
            }
            return 0;
        } else if (!strcmp(argv[i], "--memwrite")) {
            /* --memwrite <hexaddr> <hexval>: write one /dev/mem word, then exit.
             * Used to replay ICOS's CMIC MIIM bus-map registers. */
            uint32_t a, v, pg, off;
            volatile uint32_t *m;
            if (i + 2 >= argc) { usage(argv[0]); return 2; }
            a = (uint32_t)strtoul(argv[i + 1], NULL, 16);
            v = (uint32_t)strtoul(argv[i + 2], NULL, 16);
            pg = a & ~0xfffu; off = a - pg;
            m = _mmap(pg, 0x1000);
            if (m == NULL) { return 1; }
            m[off / 4] = v;
            printf("wrote 0x%08x = 0x%08x (readback 0x%08x)\n", a, v, m[off / 4]);
            return 0;
        } else if (!strcmp(argv[i], "--keep") || !strcmp(argv[i], "-k")) {
            keep = 1;
        } else if (!strcmp(argv[i], "--scan-mdio")) {
            scan = 1;
        } else if (!strcmp(argv[i], "--scan-link")) {
            scan = 2;
        } else if (!strcmp(argv[i], "--copper-up")) {
            scan = 3;
        } else if (!strcmp(argv[i], "--up-check")) {
            scan = 4;
        } else if (!strcmp(argv[i], "--try-10g")) {
            try_10g = 1;
        } else if (!strcmp(argv[i], "--l3")) {
            if (++i >= argc) { usage(argv[0]); return 2; }
            l3_conf = argv[i];
        } else if (!strcmp(argv[i], "--l3-show")) {
            l3_show_flag = 1;
        } else if (!strcmp(argv[i], "--dma-test")) {
            scan = 5;   /* exercise CPU<->chip DMA via the BDE coherent pool */
        } else if (!strcmp(argv[i], "--rx-dump")) {
            scan = 6;   /* resident RX: receive + decode frames punted to CPU */
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]); return 0;
        } else {
            LOG("unrecognized option '%s'", argv[i]);
            usage(argv[0]); return 2;
        }
    }

    if (parse_unit(unitspec, &ven, &dev, &rev, &base) < 0) {
        LOG("malformed --unit '%s'", unitspec);
        return 2;
    }

    LOG("EdgeNOS-4610 data-plane daemon starting");
    LOG("device 0x%04x:0x%04x rev 0x%02x, CMIC @ phys 0x%08x (/dev/mem)",
        ven, dev, rev, base);

    /* 1. Create CDK device (maps the CMIC). */
    if (_create_cdk_device(ven, dev, rev, base) < 0) {
        LOG("FATAL: could not create CDK device (need root / STRICT_DEVMEM off)");
        return 1;
    }

    /* NOTE on SFP+ 10G: under the default BCM56340 port config (cfg 400) the SFP+
     * ports are capped at 2.5G (forced 10G -> CDK_E_PARAM). No OpenMDK static port
     * config gives 48x1G copper AND 4x10G SFP+ together: cfg 400 = 48 copper +
     * SFP@2.5G; DCFG_4X10 -> cfg 410 = 1x20G flex (ports 50-52 disabled);
     * CHIP_FLAG_NO_HG -> cfg 497 = 4x10G but only 40 copper (41-48 disabled). The
     * board's real 48+4x10G personality (config.bcm bcm56340_4x10=1) needs the full
     * Broadcom SDK flexport bring-up, which OpenMDK's BMD lacks. So we stay on cfg
     * 400; SFP+ lasers are enabled (sfp_tx_enable) and the optical link is live, but
     * the MAC/PCS stays sub-10G pending a flexport port. */

    /* 1b. Enable external MDIO BEFORE the PHY probe: deassert the front-panel
     * PHY reset (CPLD) + program the CMIC MIIM bus map. Without this the probe
     * can't see the external 54282/84758 PHYs. */
    deassert_ext_phy_reset();
    write_icos_miim_map();

    /* 2. Probe PHY drivers + attach the BMD driver. */
#if BMD_CONFIG_INCLUDE_PHY == 1
    bmd_phy_probe_init(bmd_phy_probe_default, bmd_phy_drv_list);
#endif
    if ((rc = bmd_attach(unit)) < 0) {
        LOG("FATAL: bmd_attach: %s (%d)", CDK_ERRMSG(rc), rc);
        return 1;
    }
    LOG("[1/6] attach        OK");

    /* SFP+ 10G is enabled by patching the static port_speed_max_400 table (the SFP+
     * entries 2500->10000 in bcm56340_a0_bmd_attach.c) — a surgical change that only
     * touches the 4 SFP+ ports, unlike the dyn-config path which disturbed every
     * port's VLAN/STP model. With that table edit bmd_port_mode_set accepts 10G on
     * the SFP+; their bmd_switching_init config may still fail (TDM is nominal 2.5G),
     * which the non-fatal switching_init patch tolerates. (try_10g kept for the
     * forced-10G step below.) */
    (void)edged_pcfg_id;

    /* 3. Chip reset — sets up TOP_CORE_PLL + core clocks (MUST be before init). */
    if ((rc = bmd_reset(unit)) < 0) {
        LOG("FATAL: bmd_reset: %s (%d)", CDK_ERRMSG(rc), rc);
        return 1;
    }
    LOG("[2/6] bmd_reset     OK  (core PLL + clocks up)");

    /* CRITICAL for copper: bmd_init runs the PHY PROBE internally (bmd_phy_probe),
     * and the external 54282/84758 PHYs are only readable once the CMIC MIIM bus
     * map @0x48011000 is programmed. bmd_reset clears that map, so we must restore
     * it HERE — before bmd_init — or the probe can't see the external copper PHYs
     * and the bcm54282 driver never binds (copper stays un-driven). */
    write_icos_miim_map();

    /* 4. Chip init — block resets (IPIPE/EPIPE/ISM/AXP) over SCHAN; also probes +
     *    inits the PHYs (now that the bus map above lets it reach the externals). */
    if ((rc = bmd_init(unit)) < 0) {
        LOG("FATAL: bmd_init: %s (%d)  <- SCHAN to core failed?", CDK_ERRMSG(rc), rc);
        return 1;
    }
    LOG("[3/6] bmd_init      OK  (SCHAN core access working)");

    /* Re-assert the external MDIO bus map in case bmd_reset cleared it, so port
     * bring-up (PHY init / ucode download) can reach the external PHYs. */
    write_icos_miim_map();

    /* Copper bring-up: bmd_init's INTERNAL PHY probe ran before the bus map was
     * live (bmd_init zeroes 0x48011000 before probing), so the external BCM54282
     * copper PHYs bound the generic 'unknown' driver instead of bcm54282 — leaving
     * the copper media un-driven. Now that the bus map is restored (external MDIO
     * reads work here), re-probe + re-init the copper ports (1-48) so the real
     * bcm54282 driver binds and configures the copper side. bmd_phy_probe rebuilds
     * the whole chain (removes 'unknown', rebinds bcm54282 + the internal serdes);
     * unknown_drv is last in the list so bcm54282 gets first claim. Limited to the
     * copper range to avoid disturbing the SFP+/QSFP (84758/Warpcore) path. */
    {
        int p, reb = 0, n54282 = 0;
        for (p = 1; p <= 48; p++) {
            phy_ctrl_t *pc;
            if (bmd_phy_probe(unit, p) < 0) {
                continue;   /* not a valid/external-bus port */
            }
            for (pc = BMD_PORT_PHY_CTRL(unit, p); pc != NULL; pc = pc->next) {
                if (pc->drv && pc->drv->drv_name &&
                    !strcmp(pc->drv->drv_name, "bcm54282")) {
                    n54282++;
                }
            }
            bmd_phy_init(unit, p);
            reb++;
        }
        LOG("ext-copper: re-probed %d ports, %d now bound bcm54282", reb, n54282);
    }

    /* MDIO scan mode: bmd_init has now configured the external MDIO clock
     * (CMIC_RATE_ADJUST), so external PHYs are reachable. Scan + exit. */
    if (scan == 1) {
        scan_mdio(unit);
        return 0;
    }
    if (scan == 2) {
        scan_link(unit);
        return 0;
    }
    if (scan == 3) {
        scan_link(unit);
        copper_up(unit);
        return 0;
    }

    /* 5. L2 switching init (default VLAN 1, flood/learn). NON-FATAL: under cfg 410
     * bcm56340_a0_bmd_switching_init aborts CDK_E_PARAM when it tries to bring up
     * the unwired 20G flex ports — but by then the chip reset/init + most port
     * config have run, and step 6 below re-configures every port tolerantly. So we
     * log and proceed instead of killing the whole data plane. */
    if ((rc = bmd_switching_init(unit)) < 0) {
        LOG("[4/6] swinit        WARN rc=%s (%d) — continuing (cfg-410 flex-port "
            "abort; step 6 re-configures ports)", CDK_ERRMSG(rc), rc);
    } else {
        LOG("[4/6] swinit        OK  (L2 switching initialized)");
    }

    /* 6. Bring every valid front port up at its native speed (autoneg/Auto =
     *    per-type: 1G copper autoneg, 10G SFP+, 20G QSFP). Collect the ports
     *    that accept a mode into front_ports[] (internal ports reject it). */
    {
        cdk_pbmp_t pbmp = CDK_DEV(unit)->valid_pbmps;
        int nport = 0;

        n_front = 0;
        CDK_PBMP_ITER(pbmp, port) {
            if (port == 0) {
                continue;  /* CPU/CMIC port */
            }
            nport++;
            rc = bmd_port_mode_set(unit, port, bmdPortModeAuto, 0);
            if (rc < 0) {
                continue;  /* internal/non-front port */
            }
            if (n_front < MAX_FRONT_PORTS) {
                front_ports[n_front] = port;
                link_state[n_front] = -1;   /* unknown -> force first transition log */
                n_front++;
            }
        }
        LOG("[5/6] ports        %d/%d front ports up (native speed)", n_front, nport);

        /* Force the SFP+ ports to 10G (the static port_speed_max_400 edit makes them
         * 10G-capable). 10G fiber has no autoneg, so Auto won't pick it. Try the 10G
         * submodes; use whichever the SerDes accepts -> drives Warpcore + 84758 to
         * 10G. (try_10g no longer needed — kept as a no-op escape hatch.) */
        if (1) {
            (void)try_10g;
            /* XFI is correct for the external-84758 system side. (Tried SFI/fw_mode=2
             * 2026-06-06: no change — the blocker is upstream, the 84758 media PMD
             * not locking the fiber (sd=0), not the Warpcore interface mode.) */
            static const struct { bmd_port_mode_t m; const char *n; } x10g[] = {
                { bmdPortMode10000XFI, "XFI" }, { bmdPortMode10000SFI, "SFI" },
                { bmdPortMode10000fd,  "fd"  }, { bmdPortMode10000KR,  "KR" },
            };
            int xp, j;
            for (xp = SFP_LO; xp <= SFP_HI; xp++) {
                for (j = 0; j < (int)(sizeof(x10g)/sizeof(x10g[0])); j++) {
                    int prc = bmd_port_mode_set(unit, xp, x10g[j].m, 0);
                    if (prc >= 0) { LOG("       SFP+ port %d -> 10G %s", xp, x10g[j].n); break; }
                    if (j == (int)(sizeof(x10g)/sizeof(x10g[0])) - 1)
                        LOG("       SFP+ port %d: 10G rejected (%s)", xp, CDK_ERRMSG(prc));
                }
            }
        }
    }

    /* 7. Put every front port into STP FORWARDING so L2 actually forwards.
     *    (swinit leaves ports in the default STP state; without this they would
     *    not forward once a link comes up.) */
    {
        int nfwd = 0;
        for (i = 0; i < n_front; i++) {
            if (bmd_port_stp_set(unit, front_ports[i], bmdSpanningTreeForwarding) >= 0) {
                nfwd++;
            }
        }
        LOG("[6/6] L2 forward   %d/%d ports set to STP FORWARDING (VLAN 1)", nfwd, n_front);
    }

    /* Port inventory by speed (read back resolved modes). */
    {
        int c1g = 0, c10g = 0, c20g = 0, cother = 0;
        for (i = 0; i < n_front; i++) {
            bmd_port_mode_t mode;
            uint32_t flags = 0;
            const char *s;
            if (bmd_port_mode_get(unit, front_ports[i], &mode, &flags) < 0) {
                continue;
            }
            s = mode_str(mode);
            if      (!strcmp(s, "1G"))  c1g++;
            else if (!strcmp(s, "10G")) c10g++;
            else if (!strcmp(s, "20G")) c20g++;
            else                        cother++;
        }
        LOG("inventory: %d×1G  %d×10G  %d×20G  %d×other", c1g, c10g, c20g, cother);
    }

    /* Verify the external 84758 SFP+ PHY: it's reachable (after CPLD de-reset +
     * bus map) and its firmware checksum reg (PMA-PMD 0xca1c) reads 0x600d once
     * the ucode is loaded. 84758 is at CMIC MIIM EBUS2 addr 0-3 (cdk 0x80-0x83). */
    {
        uint32_t id1 = 0, cs = 0;
        cdk_xgsm_miim_read(unit, 0x80, (1 << 16) | 0x03, &id1);
        cdk_xgsm_miim_read(unit, 0x80, (1 << 16) | 0xca1c, &cs);
        LOG("84758(xe0) PMA-PMD ID1=0x%04x  fw-checksum(1.0xca1c)=0x%04x %s",
            id1 & 0xffff, cs & 0xffff,
            ((cs & 0xffff) == 0x600d) ? "<== UCODE LOADED" :
            ((id1 & 0xfff0) == 0x86f0) ? "(PHY reachable, ucode not yet loaded)" :
            "(PHY not reachable)");
    }

    /* Turn the SFP+ optical transmitters on (OpenMDK's bcm84740 driver leaves the
     * 84758 holding the SFP TX_DISABLE pin asserted). Without this the laser is off
     * and the link partner sees RX_LOS. */
    /* Configure-then-enable, replicating ICOS's order. KEY FINDING (ICOS capture
     * 2026-06-06): edged's media-RX stuck state (sd=0, block-lock=0) is EXACTLY
     * ICOS's port-DOWN state, and an ICOS `port en=1` re-locks it in <1s. edged
     * had enabled the port (bmd_port_mode_set) BEFORE configuring the optics, so the
     * 84758 enabled with no valid media config and latched down. Mirror ICOS:
     * (1) DISABLE the SFP+ ports, (2) configure optics while down (sfp_tx_enable),
     * (3) ENABLE -> the PHY brings up the media RX with the config already in place. */
    {
        int xp;
        LOG("SFP+ media-RX bring-up: disable -> optical config -> enable (ICOS order)");
        for (xp = SFP_LO; xp <= SFP_HI; xp++)
            bmd_port_mode_set(unit, xp, bmdPortModeDisabled, 0);
        { struct timespec ts = { .tv_sec = 0, .tv_nsec = 200*1000*1000 }; nanosleep(&ts, NULL); }

        sfp_full_reinit(unit); /* WHOLESALE robo2-order 84758 re-init (reset+re-ucode+config) */

        { struct timespec ts = { .tv_sec = 0, .tv_nsec = 200*1000*1000 }; nanosleep(&ts, NULL); }
        for (xp = SFP_LO; xp <= SFP_HI; xp++) {
            int prc = bmd_port_mode_set(unit, xp, bmdPortMode10000XFI, 0);
            if (prc < 0) bmd_port_mode_set(unit, xp, bmdPortMode10000SFI, 0);
        }

        /* Warpcore RX re-arm: the uC fw-pause handshake OpenMDK omits (the confirmed
         * gap — see SFP10G_BRINGUP_ROADMAP.md). Done AFTER the serdes is at 10G so the
         * uC re-runs RX DSC/EQ adaptation for the XFI link. */
        { struct timespec ts = { .tv_sec = 0, .tv_nsec = 200*1000*1000 }; nanosleep(&ts, NULL); }
        LOG("SFP+ Warpcore fw-pause RX re-arm (UC_CTRL 0x820e STOP/RESUME/RESTART):");
        for (xp = SFP_LO; xp <= SFP_HI; xp++)
            warpcore_fw_rearm(unit, xp);

        /* 84758 media-RX re-acquire: the LOS-toggle/EDC sequence that gives the uC the
         * LOS->present edge it needs to acquire the optical RX (the confirmed 84758-side
         * gap). Done last, after the serdes + optics are up. */
        { struct timespec ts = { .tv_sec = 0, .tv_nsec = 100*1000*1000 }; nanosleep(&ts, NULL); }
        sfp_edc_reacquire(unit);

        /* One-shot SFP+ MAC reconcile: give the optics a moment to lock, then drive
         * the BMD link state from the direct 84758 read so the XMAC un-resets and the
         * EPC_LINK_BMAP egress bit is set — without this a non---keep run never
         * forwards over the SFP+ ports (bmd_phy_link_get can't see the locked optics).
         * The --keep control loop keeps re-checking; this makes the one-shot path work. */
        { struct timespec ts = { .tv_sec = 2, .tv_nsec = 0 }; nanosleep(&ts, NULL); }
        for (xp = SFP_LO; xp <= SFP_HI; xp++) {
            int alive = sfp_link_alive(unit, xp);
            if (alive) {
                BMD_PORT_STATUS_SET(unit, xp, BMD_PST_FORCE_LINK | BMD_PST_LINK_UP);
            } else {
                BMD_PORT_STATUS_SET(unit, xp, BMD_PST_FORCE_LINK);
                BMD_PORT_STATUS_CLR(unit, xp, BMD_PST_LINK_UP);
            }
            bmd_port_mode_update(unit, xp);
            LOG("SFP+ port %d optical link %s -> MAC %s", xp,
                alive ? "UP" : "down", alive ? "forwarding" : "held");
        }
    }

    LOG("data plane UP: BCM56340 initialized, switching-ready, ports forwarding");

    /* --up-check: after the FULL bring-up (so bmd_port_mode_set has configured the
     * 54282 copper PHYs), read the real per-jack copper link directly over MDIO. */
    if (scan == 4) {
        int dports[10] = { 1, 23, 49, 50, 51, 52, 53, 54, 57, 61 };  /* copper + SFP+/QSFP */
        int k;
        for (k = 0; k < 10; k++) {
            int p = dports[k], link = -1, an = -1;
            phy_ctrl_t *pc = BMD_PORT_PHY_CTRL(unit, p);
            LOG("port %d PHY chain:", p);
            if (!pc) LOG("    (no PHY ctrl bound)");
            while (pc) {
                int plink = -2, pan = -2;
                /* per-PHY link via that driver's own pd_link_get — isolates the
                 * external 84758 from the internal Warpcore serdes. */
                if (pc->drv && pc->drv->pd_link_get) {
                    PHY_LINK_GET(pc, &plink, &pan);
                }
                LOG("    drv=%-16s bus=%-22s addr=0x%02x  link=%d an=%d",
                    pc->drv ? pc->drv->drv_name : "?",
                    pc->bus ? pc->bus->drv_name : "?",
                    PHY_CTRL_PHY_ADDR(pc), plink, pan);
                pc = pc->next;
            }
            if (bmd_phy_link_get(unit, p, &link, &an) >= 0)
                LOG("    combined bmd_phy_link_get: link=%d an_done=%d", link, an);
        }
        LOG("--up-check: waiting 5s for copper autoneg to settle...");
        { struct timespec ts = { .tv_sec = 5, .tv_nsec = 0 }; nanosleep(&ts, NULL); }
        scan_link(unit);

        /* sfp_tx_enable() already ran in the main flow; give the partner RX a
         * moment to detect light / PCS to lock, then read status. */
        { struct timespec ts = { .tv_sec = 3, .tv_nsec = 0 }; nanosleep(&ts, NULL); }

        /* SFP+ 84758 status across all MMDs at EBUS2 0x80-0x83 (xe0-3) — pinpoint
         * where the 10G chain breaks: media PMD (sees fiber light?), media PCS
         * (10GBASE-R block-lock on the fiber?), system PHY-XS (locks the Warpcore
         * XFI?). 84758 link = (PMD link) AND (Warpcore serdes link) per robo2. */
        {
            int s;
            LOG("SFP+ 84758 per-MMD status (1=PMA/PMD media, 3=PCS media, 4=PHY-XS system):");
            for (s = 0; s < 4; s++) {
                uint32_t addr = CDK_XGSM_MIIM_EBUS(2) | s;
                uint32_t pmd_st=0, pmd_sd=0, pcs_st=0, blk=0, st2pma=0, st2pcs=0, gp=0;
                uint32_t c1=0, c2=0;
                cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_PMAD_CTRL,  &c1); /* readback speed cfg */
                cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_PMAD_CTRL2, &c2);
                cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0xc820, &gp);  /* SPEED_LINK_DETECT (micro) */
                /* STAT2 (reg 8) clears latched local faults; then read status1
                 * TWICE because link-alive (bit2) is latch-low. */
                cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0x0008, &st2pma);
                cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0x0008, &st2pma);
                cdk_xgsm_miim_read(unit, addr, (3 << 16) | 0x0008, &st2pcs);
                cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0x0001, &pmd_st);
                cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0x0001, &pmd_st); /* 2nd read = current */
                cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0x000a, &pmd_sd);
                cdk_xgsm_miim_read(unit, addr, (3 << 16) | 0x0001, &pcs_st);
                cdk_xgsm_miim_read(unit, addr, (3 << 16) | 0x0001, &pcs_st); /* 2nd read */
                cdk_xgsm_miim_read(unit, addr, (3 << 16) | 0x0020, &blk);    /* 10GBASE-R PCS b0=blocklock */
                {   /* media-side config readback: vs ICOS locked (c800=3f3f c8e4=c8ef c82b=8a40 000a=1) */
                    uint32_t side=0, cmode=0, rep=0, c800=0, c8e4=0, c82b=0, sd=0;
                    cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_SIDE_SEL,   &side);
                    cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_CHIP_MODE,  &cmode);
                    cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_REPEATER_DET,&rep);
                    cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_OPT_SIGLVL, &c800);
                    cdk_xgsm_miim_read(unit, addr, (1 << 16) | X84_OPT_CFG,    &c8e4);
                    cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0xc82b,         &c82b);
                    cdk_xgsm_miim_read(unit, addr, (1 << 16) | 0x000a,         &sd);
                    LOG("  xe%d media: side=%04x(%s) c805=%04x c800=%04x c8e4=%04x c82b=%04x 000a=%04x"
                        " [ICOS locked: c800=3f3f c8e4=c8ef c82b=8a40 000a=0001]",
                        s, side&0xffff, (side&1)?"XFI":"MMF", cmode&0xffff,
                        c800&0xffff, c8e4&0xffff, c82b&0xffff, sd&0xffff);
                }
                LOG("  xe%d(0x%02x) cfg[1.0=%04x 1.7=%04x c820=%04x spd=%s] "
                    "PMD[st1=%04x st2=%04x sd=%04x LA=%d] PCS[st1=%04x st2=%04x "
                    "blklk=%04x LA=%d]%s", s, addr,
                    c1&0xffff, c2&0xffff, gp&0xffff,
                    (gp&0x44)?"10G":(gp&0x11)?"1G":"?",
                    pmd_st&0xffff, st2pma&0xffff, pmd_sd&0xffff, !!(pmd_st&0x4),
                    pcs_st&0xffff, st2pcs&0xffff, blk&0xffff, !!(pcs_st&0x4),
                    ((pmd_st&0x4)&&(pcs_st&0x4)) ? "  <== LINK (PMA&PCS LA)" :
                    (blk&0x1) ? "  <== PCS block-lock" : "");
            }
        }

        /* Does the 84758's OWN i2c master see the SFP module? Read EEPROM bytes 0-3
         * (A0h @0x50: b0=identifier 0x03=SFP, b2=connector, b3=10G compliance) via
         * the PHY BSC master. If this returns 0x03..., the PHY<->module i2c IS wired
         * (so the PHY could do real module auto-detect / read optics params); if it
         * times out, the AS4610 wires the module only to the CPU i2c-mux (optoe) and
         * the PHY can't read it -> auto-detect must stay overridden/off. */
        {
            int s;
            for (s = 0; s < 2; s++) {   /* xe0, xe1 = the fibered pair */
                uint8_t eep[4] = {0};
                int rc3 = sfp_i2c_read_via_phy(unit, s, 0x50, 0, 4, eep);
                if (rc3 == 0)
                    LOG("  xe%d PHY-i2c SFP read: %02x %02x %02x %02x  (b0 id=%s)",
                        s, eep[0], eep[1], eep[2], eep[3],
                        eep[0] == 0x03 ? "0x03 SFP -- PHY i2c WIRED to module" :
                        eep[0] == 0xff || eep[0] == 0x00 ? "empty -- PHY i2c NOT wired" : "?");
                else
                    LOG("  xe%d PHY-i2c SFP read: %s -- PHY's i2c master NOT wired to "
                        "the module (CPU i2c-mux/optoe path only)",
                        s, rc3 == -2 ? "BSC FAIL" : "timeout");
            }
        }

        /* Full 84758 line-side register dump (xe0, devad 1) for diffing against the
         * ICOS LOCKED-state capture (dumps/icos_linked_2026_06_06/23-25). Same
         * "cXXX: 0xYYYY" format. Ranges: c700-c73f, c800-c8ff, cd00-cd3f. The diff
         * (ICOS != edged, and that robo2 phy84740 WRITES) is the missing media-RX
         * config. e.g. c701 L2P lane map -- OpenMDK never sets it. */
        if (1) {
            uint32_t a = CDK_XGSM_MIIM_EBUS(2) | 0;   /* xe0 */
            int rng, r;
            static const struct { int lo, hi; } R[] = {
                {0xc700,0xc73f}, {0xc800,0xc8ff}, {0xcd00,0xcd3f}
            };
            cdk_xgsm_miim_write(unit, a, (1<<16)|X84_AER_ADDR, 0);   /* lane 0, MMF set earlier */
            LOG("=== EDGED 84758 xe0 line-side dump (diff vs ICOS 23-25) ===");
            for (rng = 0; rng < 3; rng++)
                for (r = R[rng].lo; r <= R[rng].hi; r++) {
                    uint32_t val = 0;
                    cdk_xgsm_miim_read(unit, a, (1<<16)|r, &val);
                    LOG("EDUMP %04x: 0x%04x", r, val & 0xffff);
                }
            LOG("=== end EDGED dump ===");
        }

        /* LOOPBACK ISOLATION of the WARPCORE CORE: engage the internal Warpcore's
         * OWN analog loopback (TX->RX inside the SerDes), not the outer 84758's.
         * If the Warpcore PCS then locks, the SerDes core + PLL are good and the
         * gate is downstream (84758 retiming / fiber). If it stays down, the
         * Warpcore core/RX itself isn't locking — the deep RX-cal layer. */
        /* Warpcore uC firmware liveness per SFP+ lane. OpenMDK downloads the WC40
         * microcode ONLY via the core's lane-0 port (_warpcore_primary_lane); if the
         * uC isn't running, the RX adaptation never executes -> PCS RX never locks
         * even in loopback. CRCr(0x1081fe)!=0 and VERSIONr(0x1081f0)!=0 => uC alive;
         * DOWNLOAD_STATUSr(0x10ffc5) bit0=INIT_DONE. Lane = PHY_CTRL_INST & 3. */
        {
            int xp;
            LOG("Warpcore uC liveness (per SFP+ lane):");
            for (xp = SFP_LO; xp <= SFP_HI; xp++) {
                phy_ctrl_t *pc, *w = NULL;
                uint32_t ver = 0, crc = 0, dls = 0;
                int lane;
                for (pc = BMD_PORT_PHY_CTRL(unit, xp); pc != NULL; pc = pc->next) {
                    if (pc->drv && pc->drv->drv_name &&
                        !strcmp(pc->drv->drv_name, "bcmi_warpcore_xgxs")) { w = pc; break; }
                }
                if (!w) { LOG("    port %d: no Warpcore in chain", xp); continue; }
                lane = PHY_CTRL_INST(w) & 0x3;
                phy_aer_iblk_read(w, 0x501081f0, &ver);   /* VERSIONr */
                phy_aer_iblk_read(w, 0x501081fe, &crc);   /* CRCr */
                phy_aer_iblk_read(w, 0x5010ffc5, &dls);   /* DOWNLOAD_STATUSr */
                LOG("    port %d lane%d: uC version=0x%04x CRC=0x%04x dl_status=0x%04x "
                    "INIT_DONE=%d  => uC %s", xp, lane, ver & 0xffff, crc & 0xffff,
                    dls & 0xffff, !!(dls & 0x1),
                    (crc & 0xffff) ? "ALIVE" : "NOT RUNNING");
            }
        }
        {
            int p = SFP_LO, pl = -2, pa = -2;
            phy_ctrl_t *pc, *wc = NULL;
            for (pc = BMD_PORT_PHY_CTRL(unit, p); pc != NULL; pc = pc->next) {
                if (pc->drv && pc->drv->drv_name &&
                    !strcmp(pc->drv->drv_name, "bcmi_warpcore_xgxs")) { wc = pc; break; }
            }
            LOG("Warpcore-core loopback isolation on port %d (xe0):", p);
            if (wc && wc->drv->pd_loopback_set) {
                /* WC_XGXSSTATUS (0x108001|AER_IBLK): bit11 = TX PLL lock.
                 * WC_PCS_IEEESTATUS1 (0x030001|AER_IBLK): bit2 = RX link. */
                uint32_t st = 0, pcs = 0, wcspd = 0;
                int rc2, srge;
                /* Is the Warpcore actually at 10G? RX_LINKSTATUS is PCS bit2; a 1G
                 * COMBO link with no 10G PCS lock = Warpcore stuck in 1G/SGMII
                 * (the 84758 speed_set never propagated 10G to it). Force it. */
                PHY_SPEED_GET(wc, &wcspd);
                LOG("    Warpcore speed_get = %d", (int)wcspd);
                if (wcspd != 10000) {
                    srge = PHY_SPEED_SET(wc, 10000);
                    PHY_SPEED_GET(wc, &wcspd);
                    LOG("    forced Warpcore -> 10G (rc=%d) now speed=%d", srge, (int)wcspd);
                    { struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 }; nanosleep(&ts, NULL); }
                }
                phy_aer_iblk_read(wc, 0x50108001, &st);
                phy_aer_iblk_read(wc, 0x50030001, &pcs);
                LOG("    pre-lpbk: XGXSSTATUS=%04x TXPLL_LOCK=%d  PCS_STAT1=%04x RXlink(b2)=%d",
                    st & 0xffff, !!(st & (1u<<11)), pcs & 0xffff, !!(pcs & 0x4));
                rc2 = PHY_LOOPBACK_SET(wc, 1);
                { struct timespec ts = { .tv_sec = 2, .tv_nsec = 0 }; nanosleep(&ts, NULL); }
                st = pcs = 0;
                phy_aer_iblk_read(wc, 0x50108001, &st);
                phy_aer_iblk_read(wc, 0x50030001, &pcs);
                PHY_LINK_GET(wc, &pl, &pa);
                LOG("    in-lpbk(rc=%d): XGXSSTATUS=%04x TXPLL_LOCK=%d  PCS_STAT1=%04x "
                    "RXlink=%d  drv-link=%d", rc2, st&0xffff, !!(st&(1u<<11)),
                    pcs&0xffff, !!(pcs&0x4), pl);
                PHY_LOOPBACK_SET(wc, 0);
            } else {
                LOG("    (no Warpcore phy_ctrl in chain)");
            }
        }
    }

    /* IPv4 L3 hardware forwarding: program routed interfaces + routes from the
     * config (runs after L2/port bring-up so the VLANs/ports exist). The chip
     * then routes between subnets in hardware — no CPU involvement. */
    if (l3_conf) {
        LOG("L3: programming IPv4 forwarding from %s", l3_conf);
        if (l3_config_load(unit, l3_conf) < 0) {
            LOG("L3: config had errors (see edged-l3 lines above)");
        }
    }
    if (l3_show_flag) {
        l3_show(unit);
    }

    /* --dma-test: validate the CPU punt path end to end. Inits the CMICm DMA
     * channels, allocates a buffer from the kernel-BDE coherent pool (exercising
     * edged_dma_alloc -> /dev/linux-user-bde), proves the uncached mapping is
     * CPU-accessible, TXes a broadcast frame out a port, and briefly polls RX. */
    if (scan == 5) {
        dma_addr_t baddr = 0;
        uint8_t *buf;
        int drc, j;

        LOG("--dma-test: bmd_xgsm_dma_init(unit) ...");
        drc = bmd_xgsm_dma_init(unit);
        LOG("--dma-test: dma_init rc=%d (%s)", drc, drc == 0 ? "OK" : CDK_ERRMSG(drc));

        buf = bmd_dma_alloc_coherent(unit, 2048, &baddr);
        if (buf == NULL) {
            LOG("--dma-test: bmd_dma_alloc_coherent FAILED (BDE pool not available?)");
        } else {
            volatile uint8_t *vb = buf;
            int ok = 1;
            LOG("--dma-test: coherent buf virt=%p bus=0x%08x", (void *)buf, (uint32_t)baddr);
            /* write a pattern + read it back: proves the uncached pool is mapped R/W */
            for (j = 0; j < 256; j++) vb[j] = (uint8_t)(j ^ 0xa5);
            for (j = 0; j < 256; j++) if (vb[j] != (uint8_t)(j ^ 0xa5)) { ok = 0; break; }
            LOG("--dma-test: pool R/W verify %s", ok ? "PASS" : "FAIL");

            /* Build a 64-byte broadcast test frame and TX it out the first front
             * port that is up (proves CPU->chip TX DMA pushes a frame to the wire). */
            {
                static const uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
                static const uint8_t smac[6]  = {0x00,0x11,0x22,0x33,0x44,0x55};
                bmd_pkt_t pkt;
                int tport = (n_front > 0) ? front_ports[0] : 1;
                memset(buf, 0, 64);
                memcpy(buf, bcast, 6);
                memcpy(buf + 6, smac, 6);
                buf[12] = 0x08; buf[13] = 0x00;     /* ethertype IPv4 (dummy) */
                for (j = 14; j < 64; j++) buf[j] = (uint8_t)j;
                memset(&pkt, 0, sizeof(pkt));
                pkt.port  = tport;
                pkt.data  = buf;
                pkt.size  = 64;
                pkt.baddr = baddr;
                drc = bmd_tx(unit, &pkt);
                LOG("--dma-test: bmd_tx 64B bcast out port %d rc=%d (%s)",
                    tport, drc, drc == 0 ? "SENT" : CDK_ERRMSG(drc));
            }
            bmd_dma_free_coherent(unit, 2048, buf, baddr);
        }

        /* Best-effort RX: submit a buffer + poll briefly. Without trap rules few
         * frames punt yet; a non-timeout return proves the RX DMA completion path. */
        {
            dma_addr_t rbaddr = 0;
            uint8_t *rbuf = bmd_dma_alloc_coherent(unit, 2048, &rbaddr);
            if (rbuf) {
                bmd_pkt_t rpkt, *rp = NULL;
                int rrc;
                memset(&rpkt, 0, sizeof(rpkt));
                rpkt.data = rbuf; rpkt.size = 2048; rpkt.baddr = rbaddr;
                bmd_rx_start(unit, &rpkt);
                LOG("--dma-test: RX submitted, polling 2s ...");
                for (j = 0; j < 200; j++) {
                    rrc = bmd_rx_poll(unit, &rp);
                    if (rrc == 0 && rp) {
                        LOG("--dma-test: RX got %d bytes on src_port %d", rp->size, rp->port);
                        break;
                    }
                    { struct timespec ts = { .tv_sec = 0, .tv_nsec = 10*1000*1000 }; nanosleep(&ts, NULL); }
                }
                if (j >= 200) LOG("--dma-test: no RX in 2s (expected w/o trap rules; TX path is the key result)");
                bmd_rx_stop(unit);
                bmd_dma_free_coherent(unit, 2048, rbuf, rbaddr);
            }
        }
    }

    /* --rx-dump: prove the chip->CPU RX DMA path. The CPU port is a member of the
     * default VLAN (bmd_switching_init) and bmd_rx_start enables it in EPC_LINK_BMAP,
     * so broadcast/control frames arriving on a forwarding port flood to the CPU.
     * Resident loop: submit a buffer, poll, decode, repeat. Run under an external
     * timeout; the linked Nexus emits LLDP/STP/ARP that should land here. */
    if (scan == 6) {
        dma_addr_t rbaddr = 0;
        uint8_t *rbuf;
        int got = 0, drc;

        /* Re-init the CMICm DMA channels: the (advisory) XLPORT self-test during
         * bmd_init ran a failed jumbo TX that can leave a channel mid-abort; a clean
         * re-init is what makes RX submit succeed (mirrors --dma-test). */
        drc = bmd_xgsm_dma_init(unit);
        LOG("--rx-dump: dma_init rc=%d (%s)", drc, drc == 0 ? "OK" : CDK_ERRMSG(drc));

        rbuf = bmd_dma_alloc_coherent(unit, 2048, &rbaddr);
        if (rbuf == NULL) {
            LOG("--rx-dump: DMA pool unavailable (BDE loaded?)");
        } else {
            LOG("--rx-dump: resident RX active — flood-to-CPU on default VLAN; "
                "waiting for frames (run under `timeout`, Ctrl-C to stop)");
        }
        /* TX stimulus: with the SFP+ ports physically loopbacked, a frame sent out
         * a looped port returns as ingress and floods (VLAN) to the CPU — a
         * deterministic RX trigger that doesn't depend on the link partner. */
        dma_addr_t tbaddr = 0;
        uint8_t *tbuf = rbuf ? bmd_dma_alloc_coherent(unit, 128, &tbaddr) : NULL;
        /* TX out an XLPORT/SFP+ port times out (XLPORT egress-DMA issue, separate),
         * but GE/copper TX works. Use a copper front port in MAC loopback as the RX
         * stimulus: TX a broadcast to it, the MAC loops it back as ingress, and it
         * floods VLAN 1 to the CPU — deterministic, no cabling, no XLPORT TX. */
        int loop_port = (n_front > 0) ? front_ports[0] : 1;
        int lrc = bmd_port_mode_set(unit, loop_port, bmdPortMode1000fd,
                                    BMD_PORT_MODE_F_MAC_LOOPBACK);
        bmd_vlan_port_add(unit, 1, loop_port, BMD_VLAN_PORT_F_UNTAGGED);
        bmd_port_vlan_set(unit, loop_port, 1);
        bmd_port_stp_set(unit, loop_port, bmdSpanningTreeForwarding);
        LOG("--rx-dump: stimulus port %d -> 1G MAC-loopback (rc=%d) + VLAN 1 + forwarding",
            loop_port, lrc);

        /* Arm ONE RX descriptor up front. bmd_rx_poll clears it only on an actual
         * receive; on timeout it stays armed, so we re-arm only after a frame
         * (re-arming while pending returns CDK_E_RESOURCE). */
        {
            bmd_pkt_t rpkt;
            int armed = 0, srv, rounds = 0;

            memset(&rpkt, 0, sizeof(rpkt));
            rpkt.data = rbuf; rpkt.size = 2048; rpkt.baddr = rbaddr;
            if (rbuf) {
                srv = bmd_rx_start(unit, &rpkt);
                if (srv < 0) LOG("--rx-dump: bmd_rx_start rc=%d (%s)", srv, CDK_ERRMSG(srv));
                else armed = 1;
            }

            while (armed) {
                bmd_pkt_t *rp = NULL;
                int rc2 = -1, k;

                /* fire a broadcast stimulus out the looped SFP+ port: it egresses,
                 * loops back as ingress, floods VLAN to CPU -> our armed RX. */
                if (tbuf) {
                    static const uint8_t bc[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
                    bmd_pkt_t tp;
                    int b;
                    memset(tbuf, 0, 64);
                    memcpy(tbuf, bc, 6);
                    tbuf[6]=0x02; tbuf[7]=0xed; tbuf[8]=0x60; tbuf[9]=0x00; tbuf[10]=0x00; tbuf[11]=0x01;
                    tbuf[12]=0x08; tbuf[13]=0x06;   /* ARP ethertype */
                    for (b = 14; b < 64; b++) tbuf[b] = (uint8_t)b;
                    memset(&tp, 0, sizeof(tp));
                    tp.port = loop_port; tp.data = tbuf; tp.size = 64; tp.baddr = tbaddr;
                    {
                        int trc = bmd_tx(unit, &tp);
                        if (rounds == 0) LOG("--rx-dump: stimulus bmd_tx out port %d rc=%d (%s)",
                                             loop_port, trc, trc == 0 ? "SENT" : CDK_ERRMSG(trc));
                    }
                }

                for (k = 0; k < 50; k++) {   /* poll ~0.5s, then re-stimulate */
                    rc2 = bmd_rx_poll(unit, &rp);
                    if (rc2 == 0 && rp) break;
                    { struct timespec ts = { .tv_sec = 0, .tv_nsec = 10*1000*1000 }; nanosleep(&ts, NULL); }
                }

                if (rc2 == 0 && rp) {
                    uint8_t *d = rp->data;
                    uint16_t eth = (uint16_t)((d[12] << 8) | d[13]);
                    got++;
                    LOG("RX #%d: %d bytes src_port %d  "
                        "DA %02x:%02x:%02x:%02x:%02x:%02x SA %02x:%02x:%02x:%02x:%02x:%02x "
                        "ethertype 0x%04x %s", got, rp->size, rp->port,
                        d[0],d[1],d[2],d[3],d[4],d[5], d[6],d[7],d[8],d[9],d[10],d[11],
                        eth, eth==0x0806?"ARP":eth==0x88cc?"LLDP":eth==0x0800?"IPv4":"");
                    if (got <= 2) {
                        int b; char hx[160]; int o = 0;
                        for (b = 0; b < 48 && b < rp->size; b++)
                            o += sprintf(hx + o, "%02x ", d[b]);
                        LOG("RX #%d first48: %s", got, hx);
                    }
                    /* re-arm for the next frame */
                    memset(&rpkt, 0, sizeof(rpkt));
                    rpkt.data = rbuf; rpkt.size = 2048; rpkt.baddr = rbaddr;
                    if (bmd_rx_start(unit, &rpkt) < 0) armed = 0;
                }
                if (++rounds % 20 == 0) LOG("--rx-dump: %d rounds, %d frames so far", rounds, got);
            }
        }
        bmd_rx_stop(unit);
        if (tbuf) bmd_dma_free_coherent(unit, 128, tbuf, tbaddr);
        if (rbuf) bmd_dma_free_coherent(unit, 2048, rbuf, rbaddr);
        LOG("--rx-dump: stopped (received %d frames)", got);
    }

    if (keep) {
        control_loop(unit);   /* never returns */
    }
    return 0;
}
