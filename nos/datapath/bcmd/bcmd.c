/*
 * bcmd.c — EdgeNOS-4610 datapath on the full OpenBCM SDK BCM C API.
 *
 * This file is the datapath body of the daemon. It is APPENDED to the SDK's
 * systems/linux/user/common/socdiag.c at build time (see build-bcmd.sh), and
 * socdiag's `diag_shell();` call is rewritten to `bcmd_run();`. We reuse
 * socdiag's main() (SAL init, knet_kcom_config, chip_info_vect_config,
 * bde_create + all the BDE/PCI platform hooks the libs need) and replace only
 * the interactive REPL with this deterministic datapath bring-up.
 *
 * WHY a daemon (vs hand-driving bcm.user): the diag proved the hardware works —
 * copper RX reaches the kernel, egress reaches the 54282 PHY clean (MAC+PHY
 * loopback FCS-clean) — but the diag could not reliably configure the CPU-punt
 * DMA path. Frames forwarded to the CPU port chip-side were not delivered to
 * the knet netdev (filter-2 got 3 hits vs RxAPI's 135k), so ping replies never
 * returned. See memory project_4610_fullsdk_rx_breakthrough.
 *
 * THE FIX: bcm_rx_start() with the RX DMA channel servicing ALL CPU CoS queues
 * (cos_bmp = all). Plus a static our-MAC->CPU L2 entry (so unicast replies are
 * L2-forwarded to CPU instead of bounced back out the wire — the diag let
 * dynamic learning move our MAC onto ge25) and a KNET netif+filter via the C
 * API (deterministic, unlike knetctrl).
 *
 * Run: on the box, CWD holds config.bcm, BDE + KNET modules loaded,
 * /dev/linux-bcm-knet present. socdiag's main reads config.bcm and inits SAL.
 */

#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

/* The SDK builder's <linux/i2c-dev.h> is the newer split header that lacks the
 * SMBus structs/constants, so we talk to the CPLD with raw i2c byte read/write
 * (I2C_SLAVE_FORCE + write()/read()). Define the one ioctl we need locally. */
#ifndef I2C_SLAVE_FORCE
#define I2C_SLAVE_FORCE 0x0706
#endif

#include <bcm/error.h>
#include <bcm/types.h>
#include <bcm/port.h>
#include <bcm/link.h>
#include <bcm/rx.h>
#include <bcm/knet.h>
#include <bcm/l2.h>
#include <bcm/vlan.h>
#include <bcm/stat.h>

/* ------------------------------------------------------------------ config */

#define BCMD_UNIT       0
#define BCMD_VLAN       1           /* untagged access; both ports share VLAN 1 */

/* Ports brought up SIMULTANEOUSLY, each as its own Linux netdev + CPU-punt path.
 * Add/remove rows here; bcmd loops over the table. `is_sfp` = run the 84758
 * laser/status read (SFP+ ports only). */
static const struct {
    bcm_port_t  port;       /* chip logical port */
    const char *ifname;     /* Linux netdev name */
    int         is_sfp;     /* SFP+ (84758) vs copper (54282) */
} BCMD_PORTS[] = {
    { 26, "port1",  0 },    /* ge25 = front-panel port 1, copper 54282, 10.14.1.0/24 */
    { 50, "uplink", 1 },    /* xe0  = front-panel port 49, SFP+ 84758, 10.101.102.0/29
                               (link blocked on optical overdrive — see README) */
};
#define BCMD_NPORTS ((int)(sizeof(BCMD_PORTS) / sizeof(BCMD_PORTS[0])))

/* MAC surfaced on the netdevs (shared; per-port RX is disambiguated by the
 * ingress-port KNET filter, and per-port TX by each netif's TX_LOCAL_PORT). */
static const bcm_mac_t BCMD_HOST_MAC = {0x02, 0x10, 0x18, 0x96, 0x59, 0xdd};

static volatile sig_atomic_t bcmd_g_run = 1;
static void bcmd_on_sig(int s) { (void)s; bcmd_g_run = 0; }

#define BCMD_CHK(expr) do {                                                    \
        int _rv = (expr);                                                      \
        if (_rv != BCM_E_NONE) {                                               \
            printf("[bcmd] FAIL %s = %d (%s)\n", #expr, _rv, bcm_errmsg(_rv)); \
            return _rv;                                                        \
        }                                                                      \
    } while (0)

/* --------------------------------------------------------------- chip init */

static int bcmd_chip_init(void)
{
    /* Probe + attach the SOC device via the BDE (bde_create runs inside). */
    if (sysconf_init() < 0)  { printf("[bcmd] sysconf_init failed\n");  return -1; }
    if (sysconf_probe() < 0) { printf("[bcmd] sysconf_probe failed\n"); return -1; }
    if (soc_ndev < 1)        { printf("[bcmd] no SOC devices\n");       return -1; }
    if (!soc_attached(BCMD_UNIT) && sysconf_attach(BCMD_UNIT) < 0) {
        printf("[bcmd] sysconf_attach failed\n"); return -1;
    }

    /* soc_init + bcm_init via the proven diag commands. These read config.bcm
     * (already in CWD), bring up the 56340 + 84758 firmware + QSGMII GE ports,
     * exactly as the working bcm.user sequence did. The command processor is
     * self-contained (static command table). */
    if (sh_process_command(BCMD_UNIT, "init all") != 0) {
        printf("[bcmd] 'init all' failed\n"); return -1;
    }
    if (sh_process_command(BCMD_UNIT, "init bcm") != 0) {
        printf("[bcmd] 'init bcm' failed\n"); return -1;
    }
    printf("[bcmd] chip attached + init all/bcm done\n");
    return 0;
}

/* --------------------------------------------------------------- port up */

static int bcmd_port_up(bcm_port_t port)
{
    /* SW linkscan so the MAC link tracks the 54282 PHY (without it the MAC
     * stays down though the PHY has link — the diag needed `linkscan 250000`
     * + `port geN linkscan=on`). */
    BCMD_CHK(bcm_linkscan_enable_set(BCMD_UNIT, 250000));            /* 250ms */
    BCMD_CHK(bcm_linkscan_mode_set(BCMD_UNIT, port, BCM_LINKSCAN_MODE_SW));
    BCMD_CHK(bcm_port_enable_set(BCMD_UNIT, port, 1));

    /* Disable flow control — the diag saw a continuous TX pause-frame storm
     * that backpressured the upstream trunk. */
    BCMD_CHK(bcm_port_pause_set(BCMD_UNIT, port, /*tx*/0, /*rx*/0));

    printf("[bcmd] port %d up (linkscan SW, pause off)\n", port);
    return 0;
}

/* ------------------------------------------------------------ RX + CPU punt */
/*
 * THE FIX. bcm_rx_start() arms the CPU RX DMA. The diag's `pw start` armed it
 * too, but serviced only a subset of CPU CoS queues — frames L2-forwarded to
 * the CPU (unicast replies to our MAC) landed in an unserviced queue and were
 * never DMA'd up. Map one RX DMA channel to ALL CoS queues.
 */
static int bcmd_rx_start(void)
{
    bcm_rx_cfg_t cfg;

    bcm_rx_cfg_t_init(&cfg);
    cfg.pkt_size       = 9216;       /* jumbo headroom (matches KNET rx_buf) */
    cfg.pkts_per_chain = 16;         /* continuous chained ring */
    cfg.global_pps     = 0;
    cfg.max_burst      = 0;

    /* Channel 1 services every CPU CoS queue (the fix for the punt gap). */
    cfg.chan_cfg[1].chains  = 8;
    cfg.chan_cfg[1].cos_bmp = 0xffffffff;

    BCMD_CHK(bcm_rx_start(BCMD_UNIT, &cfg));
    printf("[bcmd] RX DMA armed (all CoS -> channel 1)\n");
    return 0;
}

/* ---------------------------------------------------------- KNET netdev */
/*
 * Create one kernel netdev PER port (TX egresses that port), plus a per-port
 * ingress-port filter routing that port's CPU-bound frames to its netdev, plus
 * a low-precedence catch-all so nothing falls to the consumer-less DefaultRxAPI
 * (which would wedge the RX DMA ring). All via the C API (not knetctrl).
 *
 * Filter precedence (linux-bcm-knet sorts by priority ascending, equal priority
 * keeps creation order): all at priority 0 ("no channel check"), created
 * ingress-port filters FIRST then the catch-all LAST -> per-port filters match
 * first, catch-all is the fallback. STRIP_TAG removes the chip PVID tag so Linux
 * sees clean untagged frames (without it the netdev gets a bogus ethertype).
 */
static int bcmd_knet_setup(int netif_ids[])
{
    bcm_knet_netif_t   netif;
    bcm_knet_filter_t  filter;
    int i, p;

    BCMD_CHK(bcm_knet_init(BCMD_UNIT));

    /* Idempotent: the kernel module persists netifs/filters across processes.
     * Tear down any stale ones by id (filter 1 = auto DefaultRxAPI; leave it). */
    for (i = 2; i <= 32; i++) (void)bcm_knet_filter_destroy(BCMD_UNIT, i);
    for (i = 1; i <= 32; i++) (void)bcm_knet_netif_destroy(BCMD_UNIT, i);

    /* one TX_LOCAL_PORT netdev per port */
    for (p = 0; p < BCMD_NPORTS; p++) {
        bcm_knet_netif_t_init(&netif);
        netif.type = BCM_KNET_NETIF_T_TX_LOCAL_PORT;   /* TX egresses this port */
        netif.port = BCMD_PORTS[p].port;
        netif.vlan = BCMD_VLAN;
        sal_memcpy(netif.mac_addr, BCMD_HOST_MAC, sizeof(bcm_mac_t));
        sal_strncpy(netif.name, BCMD_PORTS[p].ifname, sizeof(netif.name) - 1);
        BCMD_CHK(bcm_knet_netif_create(BCMD_UNIT, &netif));
        netif_ids[p] = netif.id;
        printf("[bcmd] knet netif '%s' id=%d on port %d\n",
               netif.name, netif.id, BCMD_PORTS[p].port);
    }

    /* per-port ingress-port filters (created first -> matched first) */
    for (p = 0; p < BCMD_NPORTS; p++) {
        bcm_knet_filter_t_init(&filter);
        filter.type        = BCM_KNET_FILTER_T_RX_PKT;
        filter.priority    = 0;
        filter.dest_type   = BCM_KNET_DEST_T_NETIF;
        filter.dest_id     = netif_ids[p];
        filter.match_flags = BCM_KNET_FILTER_M_INGPORT;
        filter.m_ingport   = BCMD_PORTS[p].port;
        filter.flags       = BCM_KNET_FILTER_F_STRIP_TAG;
        sal_strncpy(filter.desc, BCMD_PORTS[p].ifname, sizeof(filter.desc) - 1);
        BCMD_CHK(bcm_knet_filter_create(BCMD_UNIT, &filter));
        printf("[bcmd] knet filter ingport %d -> netif %d '%s' (striptag)\n",
               BCMD_PORTS[p].port, netif_ids[p], BCMD_PORTS[p].ifname);
    }

    /* catch-all (created last -> matched last) -> first netdev, so nothing
     * accumulates on the consumer-less DefaultRxAPI and wedges the RX ring. */
    bcm_knet_filter_t_init(&filter);
    filter.type      = BCM_KNET_FILTER_T_RX_PKT;
    filter.priority  = 0;
    filter.dest_type = BCM_KNET_DEST_T_NETIF;
    filter.dest_id   = netif_ids[0];
    filter.flags     = BCM_KNET_FILTER_F_STRIP_TAG;
    sal_strncpy(filter.desc, "catch-all", sizeof(filter.desc) - 1);
    BCMD_CHK(bcm_knet_filter_create(BCMD_UNIT, &filter));
    printf("[bcmd] knet catch-all -> netif %d (fallback, striptag)\n", netif_ids[0]);
    return 0;
}

/* ---------------------------------------------------------- L2 return path */
/*
 * Static our-MAC -> CPU so unicast replies (ping echo-replies) are L2-forwarded
 * to the CPU port instead of bounced back out the wire. The diag let dynamic
 * learning move this MAC onto ge25; BCM_L2_STATIC prevents that.
 */
static int bcmd_l2_punt(void)
{
    bcm_l2_addr_t l2;

    bcm_l2_addr_t_init(&l2, BCMD_HOST_MAC, BCMD_VLAN);
    l2.flags |= BCM_L2_STATIC;
    l2.port   = 0;                  /* CPU = port 0 */
    BCMD_CHK(bcm_l2_addr_add(BCMD_UNIT, &l2));
    printf("[bcmd] L2: host MAC -> CPU (static) installed\n");
    return 0;
}

/* ------------------------------------------------------- SFP+ 84758 laser */
/*
 * Enable the SFP+ optic TX laser + optical config on the BCM84758 over the chip
 * CMIC MIIM (external bus 2 = phy_id 0x40-0x43 for xe0-3; PHY_ID_BUS_NUM encodes
 * bus 2 as bit6). The OpenBCM SDK reaches the 84758 here (its "84758 init" uses
 * the same soc_miimc45 path) but never enables the laser, because the 84758 is
 * not chained as xe0's outer port PHY (the port PHY is the internal Warpcore), so
 * phy84740's enable_set never runs. We replicate edged's sfp_tx_enable() — the
 * ICOS LOCKED values (c800=0x3f3f laser, c8e4=0xc8ef, c805 optical, 10G PMA, PCS
 * un-reset, clear TX_DISABLE, TxOnOff strap toggle). 84758 ucode is already loaded
 * by the SDK init. devad 1=PMA/PMD, 7=AN, 3=PCS.
 */
extern int soc_miimc45_write(int unit, uint16 phy_id, uint8 devad,
                             uint16 reg, uint16 data);
extern int soc_miimc45_read(int unit, uint16 phy_id, uint8 devad,
                            uint16 reg, uint16 *data);

#define X84_OPT_CFG     0xc8e4
#define X84_OPT_SIGLVL  0xc800
#define X84_TX_DISABLE  0x0009
#define X84_GENSIG_8071 0xcd16
#define X84_AER_ADDR    0xc702
#define X84_RESET_CTRL  0xcd17
#define X84_SIDE_SEL    0xffff
#define X84_CHIP_MODE   0xc805
#define X84_REPEATER_DET 0xc81d
#define X84_SQUELCH_CTL 0xcd18

static void bcmd_x84_rmw(int pid, uint8 dev, uint16 reg, uint16 clr, uint16 set)
{
    uint16 v = 0;
    if (soc_miimc45_read(BCMD_UNIT, (uint16)pid, dev, reg, &v) == 0)
        soc_miimc45_write(BCMD_UNIT, (uint16)pid, dev, reg, (uint16)((v & ~clr) | set));
}

/* READ-ONLY 84758 status. The SDK's phy84740 driver (attached during `init all`
 * + bcm_port_enable, once the CPLD reset is cleared) owns the full datapath —
 * laser, PMA type, line/system PCS, retimer/squelch. The old manual register
 * surgery below (kept #if 0 for reference) was an OpenMDK-era workaround for
 * having NO SDK PHY driver; firing it ON TOP of the SDK clobbered the RX path
 * (forced LRM PMA, reset the media PCS, flipped side-select) — frames egressed
 * but never ingressed. We now only observe; the SDK has already set c800=0x3f3f. */
static void bcmd_sfp_laser_one(int pid)
{
    uint16 c800 = 0, optcfg = 0, pmd = 0;
    soc_miimc45_read(BCMD_UNIT, (uint16)pid, 1, X84_OPT_SIGLVL, &c800);
    soc_miimc45_read(BCMD_UNIT, (uint16)pid, 1, X84_OPT_CFG,    &optcfg);
    soc_miimc45_read(BCMD_UNIT, (uint16)pid, 1, 0x0001,         &pmd);   /* PMD status1 */
    printf("[bcmd] SFP+ 84758 phy_id 0x%02x (SDK-owned): c800=0x%04x c8e4=0x%04x "
           "PMD(1.1)=0x%04x rx_link=%d\n", pid, c800, optcfg, pmd, (pmd>>2)&1);
#if 0   /* OpenMDK-era manual datapath surgery — disabled (clobbered SDK RX path) */
    soc_miimc45_write(BCMD_UNIT, pid, 1, X84_AER_ADDR, 0);
    bcmd_x84_rmw(pid, 1, X84_SIDE_SEL, 0x1, 0);
    bcmd_x84_rmw(pid, 1, X84_CHIP_MODE, (1<<3)|(1<<2), 0);
    bcmd_x84_rmw(pid, 7, 0x0000, (1<<12), 0);
    soc_miimc45_write(BCMD_UNIT, pid, 1, 0x0096, 0);
    soc_miimc45_write(BCMD_UNIT, pid, 1, 0x0000, 0x2040);
    bcmd_x84_rmw(pid, 1, 0x0007, 0xf, 0x8);
    soc_miimc45_write(BCMD_UNIT, pid, 1, X84_RESET_CTRL, 0);
    soc_miimc45_write(BCMD_UNIT, pid, 1, X84_OPT_CFG, 0xc8ef);
    soc_miimc45_write(BCMD_UNIT, pid, 1, X84_OPT_SIGLVL, 0x3f3f);
    soc_miimc45_write(BCMD_UNIT, pid, 1, 0xc82b, 0x8a40);
    bcmd_x84_rmw(pid, 1, X84_TX_DISABLE, 0x1, 0);
    bcmd_x84_rmw(pid, 1, X84_GENSIG_8071, 0x8, 0);
    bcmd_x84_rmw(pid, 1, X84_OPT_CFG, (1<<12), 0);
#endif
}

/* Scan the MIIM phy_id space for the 84758 (C45 dev1 reg2 ID0 = 0x600d) so we use
 * the SDK's real bus-encoded phy_id (port_phy_addr 0x40 != the live phy_id). */
/* ROOT CAUSE of "84758 unreachable": EdgeNOS/ONL boots with the external front-
 * panel PHYs (84758 10G SFP+ + 54282 1G copper) HELD IN RESET via CPLD GPIOs
 * (0x19=0x7f, 0x1b=0xef power-on default). The SDK's `init all` programs the CMIC
 * MIIM master correctly but CANNOT clear a board-level CPLD reset, so MIIM to the
 * external PHYs returns 0xffff. ICOS clears 0x19/0x1b=0x00. We must do the same
 * BEFORE `init all` so the SDK's PHY probe finds + attaches the 84758 (firmware,
 * serdes, link bring-up). CPLD is i2c-0 @ 0x30; use SMBus byte-data (same as the
 * bound accton_as4610_cpld driver) + read-back, FORCE past the bound driver. */
static int bcmd_cpld_read(int fd, int reg)
{
    unsigned char b = (unsigned char)reg, v = 0xff;
    if (write(fd, &b, 1) != 1) return -1;
    if (read(fd, &v, 1)  != 1) return -1;
    return v & 0xff;
}

static void bcmd_cpld_write(int fd, int reg, int val)
{
    unsigned char buf[2];
    int t;
    buf[0] = (unsigned char)reg; buf[1] = (unsigned char)val;
    for (t = 0; t < 8; t++) {
        if (write(fd, buf, 2) == 2) {
            int rb = bcmd_cpld_read(fd, reg);
            if (rb < 0 || rb == (val & 0xff)) return;   /* ok (or write-only reg) */
        }
        usleep(3000);   /* let the bound driver's poll finish, then retry */
    }
    printf("[bcmd] CPLD write 0x%02x=0x%02x not confirmed (%s)\n",
           reg, val, strerror(errno));
}

static void bcmd_deassert_ext_phy_reset(void)
{
    int fd = open("/dev/i2c-0", O_RDWR);
    int r19, r1b;
    if (fd < 0) { printf("[bcmd] open /dev/i2c-0 failed: %s\n", strerror(errno)); return; }
    if (ioctl(fd, I2C_SLAVE_FORCE, 0x30) < 0) {
        printf("[bcmd] I2C_SLAVE_FORCE 0x30 failed: %s\n", strerror(errno));
        close(fd); return;
    }
    bcmd_cpld_write(fd, 0x07, 0x02); bcmd_cpld_write(fd, 0x08, 0x02);
    bcmd_cpld_write(fd, 0x0d, 0x01);
    bcmd_cpld_write(fd, 0x19, 0x00); bcmd_cpld_write(fd, 0x1b, 0x00);
    r19 = bcmd_cpld_read(fd, 0x19); r1b = bcmd_cpld_read(fd, 0x1b);
    printf("[bcmd] ext-PHY CPLD reset deasserted: 0x19=0x%02x 0x1b=0x%02x %s\n",
           r19 & 0xff, r1b & 0xff,
           (r19 == 0 && r1b == 0) ? "(OK)" : "(WARN held)");
    close(fd);
}

/* Replay ICOS's CMIC MIIM bus-map / select registers @0x48011000 (RE'd from a
 * live ICOS dump — see edged.c write_icos_miim_map / dumps/icos_mdio_regs.txt).
 * These route the external MDIO buses to the front-panel PHY address space. */
static void bcmd_write_icos_miim_map(void)
{
    static const struct { uint32 off, val; } regs[] = {
        { 0x004, 0x00010012 }, { 0x008, 0x30001000 }, { 0x010, 0x08210001 },
        { 0x058, 0x00F00000 }, { 0x060, 0xFFFFFFFE }, { 0x064, 0x1101FFFF },
        { 0x074, 0x09249000 }, { 0x078, 0x09249249 }, { 0x07c, 0x03249249 },
        { 0x080, 0x00092480 }, { 0x084, 0x00000002 },
        { 0x094, 0x04030201 }, { 0x098, 0x08070605 }, { 0x09c, 0x0E0D0C0B },
        { 0x0a0, 0x1211100F }, { 0x0a4, 0x18171615 }, { 0x0a8, 0x1C1B1A19 },
        { 0x0ac, 0x04030201 }, { 0x0b0, 0x08070605 }, { 0x0b4, 0x0E0D0C0B },
        { 0x0b8, 0x1211100F }, { 0x0bc, 0x18171615 },
    };
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    volatile uint32 *m;
    unsigned k;
    if (fd < 0) { printf("[bcmd] open /dev/mem failed: %s\n", strerror(errno)); return; }
    m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x48011000);
    close(fd);
    if (m == MAP_FAILED) { printf("[bcmd] mmap 0x48011000 failed\n"); return; }
    for (k = 0; k < sizeof(regs)/sizeof(regs[0]); k++)
        m[regs[k].off / 4] = regs[k].val;
    munmap((void *)m, 4096);
    printf("[bcmd] wrote %u ICOS CMIC MIIM bus-map regs @0x48011000\n", k);
}

/* Hand the external MDIO to the CMIC (clear iProc MDIO-select 0x1803fc3c bit4)
 * so the chip's CMIC MIIM owns the bus and soc_miimc45 can reach the 84758. */
static void bcmd_mdio_to_cmic(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    volatile uint32 *p;
    if (fd < 0) return;
    p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x1803f000);
    close(fd);
    if (p == MAP_FAILED) return;
    printf("[bcmd] iProc MDIO-sel 0x1803fc3c before=0x%08x\n", p[0xc3c/4]);
    p[0xc3c/4] &= ~(1u << 4);                       /* CMIC = MDIO master */
    printf("[bcmd] iProc MDIO-sel 0x1803fc3c after =0x%08x (bit4 cleared)\n", p[0xc3c/4]);
    munmap((void*)p, 4096);
}

static void bcmd_sfp_scan(void)
{
    int id, n = 0;
    uint16 v;
    bcmd_mdio_to_cmic();
    printf("[bcmd] MIIM scan for 84758 (ID0=0x600d) over phy_id 0x00-0xff:\n");
    for (id = 0; id < 0x100; id++) {
        v = 0xffff;
        if (soc_miimc45_read(BCMD_UNIT, (uint16)id, 1, 0x0002, &v) == 0 &&
            v != 0xffff && v != 0x0000) {
            uint16 v1 = 0; soc_miimc45_read(BCMD_UNIT, (uint16)id, 1, 0x0003, &v1);
            printf("   phy_id 0x%02x: ID=%04x:%04x%s%s\n", id, v, v1,
                   (v == 0x600d && (v1&~0xf)==0x86f0) ? "  <== BCM84758" : "",
                   (v == 0x600d && (v1&~0xf)==0x845b) ? "  <== BCM54282" : "");
            n++;
        }
    }
    if (!n) printf("   (nothing responded on any phy_id)\n");
}

static void bcmd_sfp_laser_all(void)
{
    int p;
    bcmd_sfp_scan();
    for (p = 0; p < 4; p++) bcmd_sfp_laser_one(0x40 + p);   /* xe0-3 = phy_id 0x40-0x43 */
}

/* Log MAC link state + negotiated speed + chip RX/TX packet counters + the
 * 84758 10GBASE-R PCS block-lock, so we can see WHERE frames stop:
 *  - TX counters climbing but no RX  -> we egress, far side not replying / not
 *    decoding (PCS), or reply not ingressing.
 *  - RX climbing but netdev RX 0      -> ingress OK, CPU-punt/KNET filter wrong.
 *  - block_lock 0                     -> 10G PCS not aligned (no usable RX). */
static void bcmd_link_status(bcm_port_t port)
{
    int link = -1, speed = -1;
    uint64 rxu, rxnu, txu, txnu, rxe;
    uint16 pcs = 0xffff;
    bcm_port_link_status_get(BCMD_UNIT, port, &link);
    bcm_port_speed_get(BCMD_UNIT, port, &speed);
    COMPILER_64_ZERO(rxu); COMPILER_64_ZERO(rxnu);
    COMPILER_64_ZERO(txu); COMPILER_64_ZERO(txnu); COMPILER_64_ZERO(rxe);
    bcm_stat_get(BCMD_UNIT, port, snmpIfInUcastPkts,     &rxu);
    bcm_stat_get(BCMD_UNIT, port, snmpIfInNUcastPkts,    &rxnu);
    bcm_stat_get(BCMD_UNIT, port, snmpIfOutUcastPkts,    &txu);
    bcm_stat_get(BCMD_UNIT, port, snmpIfOutNUcastPkts,   &txnu);
    bcm_stat_get(BCMD_UNIT, port, snmpIfInErrors,        &rxe);
    /* 84758 10GBASE-R PCS status (devad 3 reg 0x0020): bit0 = rx_link/block_lock */
    soc_miimc45_read(BCMD_UNIT, 0x40, 3, 0x0020, &pcs);
    printf("[bcmd] port %d link=%s %dMb | RX uc=%u nuc=%u err=%u  TX uc=%u nuc=%u | "
           "84758 PCS(3.0020)=0x%04x block_lock=%d\n", port,
           link == BCM_PORT_LINK_STATUS_UP ? "UP" :
           link == BCM_PORT_LINK_STATUS_DOWN ? "down" : "?", speed,
           COMPILER_64_LO(rxu), COMPILER_64_LO(rxnu), COMPILER_64_LO(rxe),
           COMPILER_64_LO(txu), COMPILER_64_LO(txnu), pcs, pcs & 0x1);
}

/* ------------------------------------------------------------------- entry */
/* Called from socdiag's main() in place of diag_shell(). SAL/kcom/chip_info
 * config + config.bcm have already been done by main(). */
int bcmd_run(void)
{
    int netif_ids[BCMD_NPORTS];
    int tick = 0, p;

    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: see logs live in /tmp/bcmd.log */
    printf("[bcmd] EdgeNOS-4610 datapath daemon starting (%d ports)\n", BCMD_NPORTS);

    /* Board-level external-PHY enable — MUST run before `init all` so the SDK's
     * PHY probe finds + attaches the now-awake 84758 (firmware + serdes + link). */
    bcmd_deassert_ext_phy_reset();
    bcmd_mdio_to_cmic();
    bcmd_write_icos_miim_map();

    if (bcmd_chip_init() < 0) return 1;
    for (p = 0; p < BCMD_NPORTS; p++)
        if (bcmd_port_up(BCMD_PORTS[p].port) != BCM_E_NONE) return 1;
    bcmd_sfp_laser_all();   /* SFP+ optic TX laser on xe0-3 (no-op effect on copper) */
    if (bcmd_rx_start()       != BCM_E_NONE) return 1;
    if (bcmd_knet_setup(netif_ids) != BCM_E_NONE) return 1;
    if (bcmd_l2_punt()        != BCM_E_NONE) return 1;

    printf("[bcmd] datapath UP on %d ports:", BCMD_NPORTS);
    for (p = 0; p < BCMD_NPORTS; p++)
        printf(" %s(port%d)", BCMD_PORTS[p].ifname, BCMD_PORTS[p].port);
    printf(" — assign an IP to each netdev and ping.\n");

    signal(SIGINT,  bcmd_on_sig);
    signal(SIGTERM, bcmd_on_sig);
    for (p = 0; p < BCMD_NPORTS; p++) bcmd_link_status(BCMD_PORTS[p].port);
    while (bcmd_g_run) {
        sal_sleep(2);
        if ((++tick % 5) == 0)   /* every ~10s */
            for (p = 0; p < BCMD_NPORTS; p++) bcmd_link_status(BCMD_PORTS[p].port);
        /* TODO: reconcile netlink (RTM_NEWADDR/NEWROUTE) -> chip L3 tables,
         * ARP service. Scaffold just holds the datapath up. */
    }

    printf("[bcmd] shutting down\n");
    bcm_rx_stop(BCMD_UNIT, NULL);
    return 0;
}
