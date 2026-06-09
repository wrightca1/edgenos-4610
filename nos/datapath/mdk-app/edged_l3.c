/*
 * edged_l3.c — IPv4 L3 hardware forwarding for the AS4610-54T (BCM56340 Helix4).
 * See edged_l3.h for the model. All chip access is via the source-available CDK
 * memory accessors generated in bcm56340_a0_defs.h; BMD owns L2/VLAN.
 */
#include <bmd_config.h>
#include <bmd/bmd.h>
#include <bmd/bmd_device.h>

#include <cdk/chip/bcm56340_a0_defs.h>
#include <cdk/cdk_field.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "edged_l3.h"

#define L3LOG(fmt, ...) fprintf(stderr, "edged-l3: " fmt "\n", ##__VA_ARGS__)

/* Single global VRF / single module on a one-chip box. */
#define L3_VRF        0
#define L3_VRF_MASK   0x7ff
#define MY_MODID      0

/* Table index allocators. Low indices are well within every table's range
 * (DEFIP/NEXT_HOP are thousands, MY_STATION/EGR_L3_INTF hundreds+). Index 0 of
 * the next-hop tables is reserved as the "null" next hop, so allocation starts
 * at 1 there; EGR_L3_INTF likewise reserves 0. */
static int next_intf_idx = 1;   /* EGR_L3_INTF */
static int next_nh_idx   = 1;   /* EGR/ING_L3_NEXT_HOP (shared index space) */
static int next_ms_idx   = 0;   /* MY_STATION_TCAM */
static int next_defip_idx = 0;  /* L3_DEFIP (one IPv4 route per index, half 0) */

/* Remember programmed entries for l3_show() and for nbr/route resolution. */
#define MAX_NBR 64
static struct {
    uint32_t ip;        /* host order */
    int      nh_idx;
    int      defip_idx; /* /32 host route slot */
    int      valid;
} nbr_tab[MAX_NBR];
static int n_nbr;

#define MAX_INTF 64
static struct { int vlan; int port; int intf_idx; int valid; } intf_tab[MAX_INTF];
static int n_intf;

#define MAX_ROUTE 256
static struct { uint32_t prefix; int len; int nh_idx; int defip_idx; } route_tab[MAX_ROUTE];
static int n_route;

/* Force the XMAC RX/TX enable on a port: bmd_port_mode_update only toggles
 * SOFT_RESET, and a port whose mode resolved to "other" can be left with RX_EN=0
 * (so it TXes but never receives — MAC rx_pkts stays 0). Returns the XMAC_CTRL
 * value after, or 0xffffffff on I/O error. Reports before/after. */
uint32_t
l3_mac_rx_enable(int unit, int port)
{
    XMAC_CTRLr_t c;
    XMAC_MODEr_t m;
    uint32_t before, after, spd;
    int ioerr = 0;
    ioerr += READ_XMAC_CTRLr(unit, port, &c);
    before = XMAC_CTRLr_GET(c, 0);
    XMAC_CTRLr_SOFT_RESETf_SET(c, 0);
    XMAC_CTRLr_RX_ENf_SET(c, 1);
    XMAC_CTRLr_TX_ENf_SET(c, 1);
    /* THE copper-RX gate (ICOS-verified, 2026-06-07): on these QSGMII GE ports the
     * XMAC takes its link state from SW_LINK_STATUS. bmd leaves it 0 -> the MAC treats
     * the link as DOWN and drops ALL ingress (TX is unaffected), so rx_pkts stays 0
     * even with the serdes linked. ICOS sets XMAC_CTRL=0x1803 (SW_LINK_STATUS=1 +
     * XGMII_IPG_CHECK_DISABLE=1) vs our 0x0003. Match it. */
    XMAC_CTRLr_SW_LINK_STATUSf_SET(c, 1);
    XMAC_CTRLr_XGMII_IPG_CHECK_DISABLEf_SET(c, 1);
    ioerr += WRITE_XMAC_CTRLr(unit, port, c);
    ioerr += READ_XMAC_CTRLr(unit, port, &c);
    after = XMAC_CTRLr_GET(c, 0);
    /* Also report the MAC speed: SPEED_MODE 0=10M 1=100M 2=1G 3=2.5G 4=10G. A copper
     * QSGMII port must be at 1G (2); a mismatch silently drops RX. */
    ioerr += READ_XMAC_MODEr(unit, port, &m);
    spd = XMAC_MODEr_SPEED_MODEf_GET(m);
    L3LOG("port %d XMAC_CTRL %08x -> %08x [rx_en=%d tx_en=%d soft_rst=%d] SPEED_MODE=%u (%s)",
          port, before, after, !!(after & 0x2), !!(after & 0x1), !!(after & 0x40),
          spd, spd==2?"1G":spd==4?"10G":spd==1?"100M":spd==0?"10M":"?");
    return ioerr ? 0xffffffff : after;
}

/*
 * Read + log the XMAC RX link-status-signaling state. For a 1G SGMII/QSGMII copper
 * port the 10G-style LSS faults (local/remote/link-interruption) must be DISABLED —
 * else the XMAC sits in a fault state and silently drops all RX while TX still works,
 * which is exactly the chip-serdes-link-up-but-rx_pkts=0 symptom.
 */
void
l3_mac_rx_diag(int unit, int port)
{
    XMAC_RX_LSS_STATUSr_t st;
    XMAC_RX_LSS_CTRLr_t ct;
    int ioerr = 0;
    uint32_t lf, rf, li, lfd, rfd, lid, stv, ctv;

    ioerr += READ_XMAC_RX_LSS_STATUSr(unit, port, &st);
    stv = XMAC_RX_LSS_STATUSr_GET(st, 0);
    lf  = XMAC_RX_LSS_STATUSr_LOCAL_FAULT_STATUSf_GET(st);
    rf  = XMAC_RX_LSS_STATUSr_REMOTE_FAULT_STATUSf_GET(st);
    li  = XMAC_RX_LSS_STATUSr_LINK_INTERRUPTION_STATUSf_GET(st);
    ioerr += READ_XMAC_RX_LSS_CTRLr(unit, port, &ct);
    ctv = XMAC_RX_LSS_CTRLr_GET(ct, 0);
    lfd = XMAC_RX_LSS_CTRLr_LOCAL_FAULT_DISABLEf_GET(ct);
    rfd = XMAC_RX_LSS_CTRLr_REMOTE_FAULT_DISABLEf_GET(ct);
    lid = XMAC_RX_LSS_CTRLr_LINK_INTERRUPTION_DISABLEf_GET(ct);

    L3LOG("port %d RX-diag: LSS_STATUS=%08x [local_fault=%u remote_fault=%u link_intr=%u] "
          "LSS_CTRL=%08x [lf_dis=%u rf_dis=%u li_dis=%u] %s",
          port, stv, lf, rf, li, ctv, lfd, rfd, lid,
          ((lf && !lfd) || (rf && !rfd) || (li && !lid)) ?
              "<== FAULT active + NOT disabled — XMAC drops RX!" : "");
    (void)ioerr;
}

/*
 * THE candidate fix: bcm56340 bmd_init hardcodes PORT_MODE_REG.XPCn_GMII_MII_ENABLE=0
 * for every port (incl the 1G QSGMII GE ports), but the full SDK sets it to 1 for any
 * sub-10G port (xmac.c: PORT_GMII_MII_ENABLE = speed>=10000 ? 0 : 1). With it 0 the
 * XMAC runs in 10G XGMII framing and cannot decode the 1G GMII RX stream coming off the
 * QSGMII deframer -> rx_pkts stays 0 even though the serdes links and TX works.
 * Set all three XPC GMII_MII_ENABLE bits in this port's PORT_MODE_REG to 1 (the GE block
 * is all 1G), with an XMAC soft-reset around the write so the mode change takes effect.
 * Logs before/after. Returns PORT_MODE_REG after.
 */
uint32_t
l3_mac_gmii_enable(int unit, int port)
{
    PORT_MODE_REGr_t pm;
    XMAC_CTRLr_t c;
    uint32_t before, after;
    int ioerr = 0;

    ioerr += READ_PORT_MODE_REGr(unit, &pm, port);
    before = PORT_MODE_REGr_GET(pm);
    L3LOG("port %d PORT_MODE_REG=%08x [xpc0_gmii=%u xpc1_gmii=%u xpc2_gmii=%u "
          "xp0_phy=%u xp0_core=%u]", port, before,
          PORT_MODE_REGr_XPC0_GMII_MII_ENABLEf_GET(pm),
          PORT_MODE_REGr_XPC1_GMII_MII_ENABLEf_GET(pm),
          PORT_MODE_REGr_XPC2_GMII_MII_ENABLEf_GET(pm),
          PORT_MODE_REGr_XPORT0_PHY_PORT_MODEf_GET(pm),
          PORT_MODE_REGr_XPORT0_CORE_PORT_MODEf_GET(pm));

    PORT_MODE_REGr_XPC0_GMII_MII_ENABLEf_SET(pm, 1);
    PORT_MODE_REGr_XPC1_GMII_MII_ENABLEf_SET(pm, 1);
    PORT_MODE_REGr_XPC2_GMII_MII_ENABLEf_SET(pm, 1);

    /* XMAC soft-reset around the mode change. */
    ioerr += READ_XMAC_CTRLr(unit, port, &c);
    XMAC_CTRLr_SOFT_RESETf_SET(c, 1);
    ioerr += WRITE_XMAC_CTRLr(unit, port, c);
    ioerr += WRITE_PORT_MODE_REGr(unit, pm, port);
    XMAC_CTRLr_SOFT_RESETf_SET(c, 0);
    XMAC_CTRLr_RX_ENf_SET(c, 1);
    XMAC_CTRLr_TX_ENf_SET(c, 1);
    ioerr += WRITE_XMAC_CTRLr(unit, port, c);

    ioerr += READ_PORT_MODE_REGr(unit, &pm, port);
    after = PORT_MODE_REGr_GET(pm);
    L3LOG("port %d PORT_MODE_REG -> %08x [xpc0_gmii=%u] (GMII_MII_ENABLE set)",
          port, after, PORT_MODE_REGr_XPC0_GMII_MII_ENABLEf_GET(pm));
    return ioerr ? 0xffffffff : after;
}

/* Force-disable the XMAC RX LSS faults so a 1G SGMII port stops dropping RX on
 * phantom 10G faults. Returns LSS_CTRL after. */
uint32_t
l3_mac_lss_disable(int unit, int port)
{
    XMAC_RX_LSS_CTRLr_t ct;
    int ioerr = 0;
    uint32_t after;
    ioerr += READ_XMAC_RX_LSS_CTRLr(unit, port, &ct);
    XMAC_RX_LSS_CTRLr_LOCAL_FAULT_DISABLEf_SET(ct, 1);
    XMAC_RX_LSS_CTRLr_REMOTE_FAULT_DISABLEf_SET(ct, 1);
    XMAC_RX_LSS_CTRLr_LINK_INTERRUPTION_DISABLEf_SET(ct, 1);
    ioerr += WRITE_XMAC_RX_LSS_CTRLr(unit, port, ct);
    ioerr += READ_XMAC_RX_LSS_CTRLr(unit, port, &ct);
    after = XMAC_RX_LSS_CTRLr_GET(ct, 0);
    L3LOG("port %d LSS faults disabled -> LSS_CTRL=%08x", port, after);
    return ioerr ? 0xffffffff : after;
}

/* phys -> logical port (the number the ingress pipeline / PBMP use). */
static int
p2l(int unit, int port)
{
    if (BMD_PORT_P2L(unit)) {
        return BMD_PORT_P2L(unit)(unit, port, 0);
    }
    return port;
}

/* Pack a 6-byte MAC into the 2-word little-endian array cdk_field_set wants:
 * w[0] = low 32 bits of the 48-bit field, w[1] = high 16 bits. mac[0] is the
 * most-significant byte of the field (standard Broadcom MAC ordering). */
static void
mac_words(const uint8_t mac[6], uint32_t w[2])
{
    w[0] = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
           ((uint32_t)mac[4] << 8)  |  (uint32_t)mac[5];
    w[1] = ((uint32_t)mac[0] << 8)  |  (uint32_t)mac[1];
}

static const char *
mac_str(const uint8_t m[6])
{
    static char b[18];
    sprintf(b, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

/* ----------------------------------------------------------------------------
 * L3 interface
 * --------------------------------------------------------------------------*/
int
l3_intf_create(int unit, int vlan, int port, const uint8_t mac[6])
{
    int ioerr = 0;
    int lport = p2l(unit, port);
    int intf_idx = next_intf_idx++;
    int ms_idx = next_ms_idx++;
    uint32_t macw[2];
    EGR_L3_INTFm_t egr_intf;
    MY_STATION_TCAMm_t ms;
    L3_IIFm_t iif;
    PORT_TABm_t port_tab;

    mac_words(mac, macw);

    /* L2: VLAN with this port as an untagged (access) member + PVID. BMD owns it. */
    bmd_vlan_create(unit, vlan);
    bmd_vlan_port_add(unit, vlan, port, BMD_VLAN_PORT_F_UNTAGGED);
    bmd_port_vlan_set(unit, port, vlan);

    /* Egress L3 interface: source MAC + egress VLAN the routed frame leaves on. */
    EGR_L3_INTFm_CLR(egr_intf);
    EGR_L3_INTFm_MAC_ADDRESSf_SET(egr_intf, macw);
    EGR_L3_INTFm_VIDf_SET(egr_intf, vlan);
    EGR_L3_INTFm_TTL_THRESHOLDf_SET(egr_intf, 0);
    ioerr += WRITE_EGR_L3_INTFm(unit, intf_idx, egr_intf);

    /* MY_STATION_TCAM: a frame whose DA == router MAC on this VLAN is a routing
     * candidate (allow IPv4 termination -> L3 lookup). Exact MAC + VLAN match. */
    MY_STATION_TCAMm_CLR(ms);
    MY_STATION_TCAMm_VALIDf_SET(ms, 1);
    MY_STATION_TCAMm_MAC_ADDRf_SET(ms, macw);
    {
        uint32_t allmac[2] = { 0xffffffff, 0x0000ffff };
        MY_STATION_TCAMm_MAC_ADDR_MASKf_SET(ms, allmac);
    }
    MY_STATION_TCAMm_VLAN_IDf_SET(ms, vlan);
    MY_STATION_TCAMm_VLAN_ID_MASKf_SET(ms, 0xfff);
    MY_STATION_TCAMm_IPV4_TERMINATION_ALLOWEDf_SET(ms, 1);
    ioerr += WRITE_MY_STATION_TCAMm(unit, ms_idx, ms);

    /* Ingress interface -> VRF 0, allow routing to the global route table. */
    READ_L3_IIFm(unit, vlan, &iif);
    L3_IIFm_VRFf_SET(iif, L3_VRF);
    L3_IIFm_ALLOW_GLOBAL_ROUTEf_SET(iif, 1);
    ioerr += WRITE_L3_IIFm(unit, vlan, iif);

    /* Enable IPv4 L3 lookup on the ingress port. */
    ioerr += READ_PORT_TABm(unit, lport, &port_tab);
    PORT_TABm_V4L3_ENABLEf_SET(port_tab, 1);
    ioerr += WRITE_PORT_TABm(unit, lport, port_tab);

    if (ioerr) {
        L3LOG("intf vlan %d port %d: I/O error (%d)", vlan, port, ioerr);
        return -1;
    }
    if (n_intf < MAX_INTF) {
        intf_tab[n_intf].vlan = vlan;
        intf_tab[n_intf].port = port;
        intf_tab[n_intf].intf_idx = intf_idx;
        intf_tab[n_intf].valid = 1;
        n_intf++;
    }
    L3LOG("intf #%d: vlan %d port %d (lport %d) mac %s -> EGR_L3_INTF[%d], "
          "MY_STATION[%d], PORT_TAB[%d].V4L3_ENABLE=1",
          intf_idx, vlan, port, lport, mac_str(mac), intf_idx, ms_idx, lport);
    return intf_idx;
}

/* ----------------------------------------------------------------------------
 * Next hop
 * --------------------------------------------------------------------------*/
int
l3_nexthop_create(int unit, const uint8_t dmac[6], int egr_port, int egr_intf_idx)
{
    int ioerr = 0;
    int nh_idx = next_nh_idx++;
    int egr_lport = p2l(unit, egr_port);
    uint32_t macw[2];
    EGR_L3_NEXT_HOPm_t egr_nh;
    ING_L3_NEXT_HOPm_t ing_nh;

    mac_words(dmac, macw);

    /* Egress next hop (L3 unicast view, ENTRY_TYPE 0): rewrite DA + point at the
     * egress L3 interface (which supplies SA + egress VLAN). */
    EGR_L3_NEXT_HOPm_CLR(egr_nh);
    EGR_L3_NEXT_HOPm_ENTRY_TYPEf_SET(egr_nh, 0);
    EGR_L3_NEXT_HOPm_L3_INTF_NUMf_SET(egr_nh, egr_intf_idx);
    EGR_L3_NEXT_HOPm_L3_MAC_ADDRESSf_SET(egr_nh, macw);
    ioerr += WRITE_EGR_L3_NEXT_HOPm(unit, nh_idx, egr_nh);

    /* Ingress next hop: where the routed copy is sent — local egress port +
     * module, not a trunk. ENTRY_TYPE 0 = single L3 unicast port. */
    ING_L3_NEXT_HOPm_CLR(ing_nh);
    ING_L3_NEXT_HOPm_ENTRY_TYPEf_SET(ing_nh, 0);
    ING_L3_NEXT_HOPm_PORT_NUMf_SET(ing_nh, egr_lport);
    ING_L3_NEXT_HOPm_MODULE_IDf_SET(ing_nh, MY_MODID);
    ING_L3_NEXT_HOPm_Tf_SET(ing_nh, 0);
    ioerr += WRITE_ING_L3_NEXT_HOPm(unit, nh_idx, ing_nh);

    if (ioerr) {
        L3LOG("nexthop dmac %s port %d: I/O error (%d)", mac_str(dmac), egr_port, ioerr);
        return -1;
    }
    L3LOG("nexthop #%d: dmac %s out port %d (lport %d) via EGR_L3_INTF[%d]",
          nh_idx, mac_str(dmac), egr_port, egr_lport, egr_intf_idx);
    return nh_idx;
}

/* ----------------------------------------------------------------------------
 * Route (L3_DEFIP LPM)
 * --------------------------------------------------------------------------*/
int
l3_route_add(int unit, uint32_t prefix, int len, int nh_idx)
{
    int ioerr = 0;
    int idx = next_defip_idx++;
    uint32_t mask = (len <= 0) ? 0u :
                    (len >= 32) ? 0xffffffffu : (0xffffffffu << (32 - len));
    L3_DEFIPm_t e;

    L3_DEFIPm_CLR(e);
    /* Half 0 = one IPv4 unicast route. MODE 0 = IPv4. */
    L3_DEFIPm_VALID0f_SET(e, 1);
    L3_DEFIPm_MODE0f_SET(e, 0);
    L3_DEFIPm_MODE_MASK0f_SET(e, 0x3);
    L3_DEFIPm_IP_ADDR0f_SET(e, prefix & mask);
    L3_DEFIPm_IP_ADDR_MASK0f_SET(e, mask);
    L3_DEFIPm_VRF_ID_0f_SET(e, L3_VRF);
    L3_DEFIPm_VRF_ID_MASK0f_SET(e, L3_VRF_MASK);
    L3_DEFIPm_NEXT_HOP_INDEX0f_SET(e, nh_idx);
    L3_DEFIPm_ECMP0f_SET(e, 0);
    if (len <= 0) {
        L3_DEFIPm_DEFAULTROUTE0f_SET(e, 1);
    }
    ioerr += WRITE_L3_DEFIPm(unit, idx, e);

    if (ioerr) {
        L3LOG("route %u.%u.%u.%u/%d: I/O error (%d)",
              (prefix >> 24) & 0xff, (prefix >> 16) & 0xff,
              (prefix >> 8) & 0xff, prefix & 0xff, len, ioerr);
        return -1;
    }
    if (n_route < MAX_ROUTE) {
        route_tab[n_route].prefix = prefix & mask;
        route_tab[n_route].len = len;
        route_tab[n_route].nh_idx = nh_idx;
        route_tab[n_route].defip_idx = idx;
        n_route++;
    }
    L3LOG("route %u.%u.%u.%u/%d -> nh #%d  L3_DEFIP[%d]",
          (prefix >> 24) & 0xff, (prefix >> 16) & 0xff,
          (prefix >> 8) & 0xff, prefix & 0xff, len, nh_idx, idx);
    return idx;
}

/* ----------------------------------------------------------------------------
 * Read-back / verification
 * --------------------------------------------------------------------------*/
void
l3_show(int unit)
{
    int i;
    L3LOG("=== L3 table read-back ===");
    for (i = 0; i < n_intf; i++) {
        EGR_L3_INTFm_t e;
        uint32_t macw[2] = {0,0}, vid;
        if (!intf_tab[i].valid) continue;
        READ_EGR_L3_INTFm(unit, intf_tab[i].intf_idx, &e);
        EGR_L3_INTFm_MAC_ADDRESSf_GET(e, macw);
        vid = EGR_L3_INTFm_VIDf_GET(e);
        L3LOG("  EGR_L3_INTF[%d]: vid=%u mac=%04x%08x  (intf vlan %d port %d)",
              intf_tab[i].intf_idx, vid, macw[1], macw[0],
              intf_tab[i].vlan, intf_tab[i].port);
    }
    for (i = 0; i < n_nbr; i++) {
        EGR_L3_NEXT_HOPm_t en;
        ING_L3_NEXT_HOPm_t in;
        uint32_t macw[2] = {0,0}, intf, port_num;
        if (!nbr_tab[i].valid) continue;
        READ_EGR_L3_NEXT_HOPm(unit, nbr_tab[i].nh_idx, &en);
        READ_ING_L3_NEXT_HOPm(unit, nbr_tab[i].nh_idx, &in);
        EGR_L3_NEXT_HOPm_L3_MAC_ADDRESSf_GET(en, macw);
        intf = EGR_L3_NEXT_HOPm_L3_INTF_NUMf_GET(en);
        port_num = ING_L3_NEXT_HOPm_PORT_NUMf_GET(in);
        L3LOG("  NEXT_HOP[%d]: dmac=%04x%08x intf=%u egress_lport=%u  (ip %u.%u.%u.%u)",
              nbr_tab[i].nh_idx, macw[1], macw[0], intf, port_num,
              (nbr_tab[i].ip >> 24) & 0xff, (nbr_tab[i].ip >> 16) & 0xff,
              (nbr_tab[i].ip >> 8) & 0xff, nbr_tab[i].ip & 0xff);
    }
    for (i = 0; i < n_route; i++) {
        L3_DEFIPm_t e;
        uint32_t ip, msk, nh, valid;
        READ_L3_DEFIPm(unit, route_tab[i].defip_idx, &e);
        valid = L3_DEFIPm_VALID0f_GET(e);
        ip = L3_DEFIPm_IP_ADDR0f_GET(e);
        msk = L3_DEFIPm_IP_ADDR_MASK0f_GET(e);
        nh = L3_DEFIPm_NEXT_HOP_INDEX0f_GET(e);
        L3LOG("  L3_DEFIP[%d]: valid=%u ip=%08x mask=%08x nh=%u  (%u.%u.%u.%u/%d)",
              route_tab[i].defip_idx, valid, ip, msk, nh,
              (route_tab[i].prefix >> 24) & 0xff, (route_tab[i].prefix >> 16) & 0xff,
              (route_tab[i].prefix >> 8) & 0xff, route_tab[i].prefix & 0xff,
              route_tab[i].len);
    }
    L3LOG("=== end read-back ===");
}

/* ----------------------------------------------------------------------------
 * Config file
 * --------------------------------------------------------------------------*/
static int
parse_mac(const char *s, uint8_t mac[6])
{
    unsigned v[6];
    int i;
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return -1;
    for (i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
    return 0;
}

static int
parse_ip(const char *s, uint32_t *ip)
{
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return -1;
    *ip = (a << 24) | (b << 16) | (c << 8) | d;
    return 0;
}

static int
intf_idx_for_vlan(int vlan)
{
    int i;
    for (i = 0; i < n_intf; i++)
        if (intf_tab[i].valid && intf_tab[i].vlan == vlan) return intf_tab[i].intf_idx;
    return -1;
}

static int
port_for_vlan(int vlan)
{
    int i;
    for (i = 0; i < n_intf; i++)
        if (intf_tab[i].valid && intf_tab[i].vlan == vlan) return intf_tab[i].port;
    return -1;
}

static int
nh_for_ip(uint32_t ip)
{
    int i;
    for (i = 0; i < n_nbr; i++)
        if (nbr_tab[i].valid && nbr_tab[i].ip == ip) return nbr_tab[i].nh_idx;
    return -1;
}

int
l3_config_load(int unit, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    int lineno = 0, errors = 0;

    if (!f) {
        L3LOG("config: cannot open %s", path);
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        char kw[32], a1[64], a2[64], a3[64];
        int n;
        lineno++;
        if (line[0] == '#' || line[0] == '\n') continue;
        n = sscanf(line, "%31s %63s %63s %63s", kw, a1, a2, a3);
        if (n < 1) continue;

        if (!strcmp(kw, "intf") && n >= 4) {
            int vlan = atoi(a1), port = atoi(a2);
            uint8_t mac[6];
            if (parse_mac(a3, mac) < 0) { L3LOG("line %d: bad mac %s", lineno, a3); errors++; continue; }
            if (l3_intf_create(unit, vlan, port, mac) < 0) errors++;
        } else if (!strcmp(kw, "nbr") && n >= 4) {
            uint32_t ip;
            uint8_t mac[6];
            int vlan = atoi(a3), egr_intf, egr_port, nh;
            if (parse_ip(a1, &ip) < 0)  { L3LOG("line %d: bad ip %s", lineno, a1); errors++; continue; }
            if (parse_mac(a2, mac) < 0) { L3LOG("line %d: bad mac %s", lineno, a2); errors++; continue; }
            egr_intf = intf_idx_for_vlan(vlan);
            egr_port = port_for_vlan(vlan);
            if (egr_intf < 0 || egr_port < 0) {
                L3LOG("line %d: nbr via unknown vlan %d (define 'intf' first)", lineno, vlan);
                errors++; continue;
            }
            nh = l3_nexthop_create(unit, mac, egr_port, egr_intf);
            if (nh < 0) { errors++; continue; }
            /* directly-connected neighbor => /32 host route */
            if (l3_route_add(unit, ip, 32, nh) < 0) { errors++; continue; }
            if (n_nbr < MAX_NBR) {
                nbr_tab[n_nbr].ip = ip;
                nbr_tab[n_nbr].nh_idx = nh;
                nbr_tab[n_nbr].defip_idx = next_defip_idx - 1;
                nbr_tab[n_nbr].valid = 1;
                n_nbr++;
            }
        } else if (!strcmp(kw, "route") && n >= 3) {
            uint32_t prefix, via;
            int len = 32, nh;
            char *slash = strchr(a1, '/');
            if (slash) { *slash = 0; len = atoi(slash + 1); }
            if (parse_ip(a1, &prefix) < 0) { L3LOG("line %d: bad prefix %s", lineno, a1); errors++; continue; }
            if (parse_ip(a2, &via) < 0)    { L3LOG("line %d: bad via %s", lineno, a2); errors++; continue; }
            nh = nh_for_ip(via);
            if (nh < 0) { L3LOG("line %d: route via unknown nbr %s (define 'nbr' first)", lineno, a2); errors++; continue; }
            if (l3_route_add(unit, prefix, len, nh) < 0) errors++;
        } else {
            L3LOG("line %d: unrecognized: %s", lineno, line);
            errors++;
        }
    }
    fclose(f);
    L3LOG("config: %s loaded (%d intf, %d nbr, %d route, %d errors)",
          path, n_intf, n_nbr, n_route, errors);
    return errors ? -1 : 0;
}
