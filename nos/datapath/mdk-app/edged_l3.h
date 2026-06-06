/*
 * edged_l3.h — IPv4 L3 hardware forwarding for the AS4610-54T (BCM56340 Helix4).
 *
 * Programs the chip's L3 forwarding chain directly through the CDK memory
 * accessors (WRITE_*m + field setters from bcm56340_a0_defs.h). BMD owns L2/VLAN;
 * this owns the routed path:
 *
 *   ingress port  --PORT_TAB.V4L3_ENABLE-->  MY_STATION_TCAM (router MAC hit)
 *     --> L3_IIF (VRF) --> L3_DEFIP (LPM route) --> next-hop index
 *     --> ING_L3_NEXT_HOP (egress port/module) + EGR_L3_NEXT_HOP (dst MAC rewrite)
 *     --> EGR_L3_INTF (src MAC + egress VLAN) --> egress port
 *
 * Single global VRF (0), single module (0). IPv4 unicast only for now.
 */
#ifndef EDGED_L3_H
#define EDGED_L3_H

#include <stdint.h>

/*
 * Create a routed interface on (vlan, port) with router MAC `mac`.
 * Sets up the VLAN (BMD: create + add port untagged + PVID), then programs
 * EGR_L3_INTF (src MAC + egress VLAN), MY_STATION_TCAM (router MAC + VLAN ->
 * allow IPv4 termination), L3_IIF[vlan] (VRF 0), and PORT_TAB.V4L3_ENABLE on the
 * ingress port. Returns the EGR_L3_INTF index (>=0) or <0 on error.
 */
int l3_intf_create(int unit, int vlan, int port, const uint8_t mac[6]);

/*
 * Create a next hop: neighbor `dmac` reachable out `egr_port`, rewriting via the
 * egress interface `egr_intf_idx` (from l3_intf_create). Programs EGR_L3_NEXT_HOP
 * (dst MAC + intf) and ING_L3_NEXT_HOP (egress port + module) at a shared index.
 * Returns the next-hop index (>=0) or <0 on error.
 */
int l3_nexthop_create(int unit, const uint8_t dmac[6], int egr_port,
                      int egr_intf_idx);

/*
 * Add an LPM route prefix/len -> next-hop index into L3_DEFIP (VRF 0).
 * `prefix` is host byte order (e.g. 0x0a000000 for 10.0.0.0). len 0..32.
 * A /32 acts as a host route. Returns the DEFIP index used (>=0) or <0.
 */
int l3_route_add(int unit, uint32_t prefix, int len, int nh_idx);

/* Read back and print every L3 table entry programmed so far (verification). */
void l3_show(int unit);

/*
 * Load and program an L3 config file. Format (whitespace-separated, # comments):
 *   intf  <vlan> <port> <router-mac>           e.g.  intf 11 53 00:11:22:33:44:55
 *   nbr   <ip>   <mac>  <via-vlan>              e.g.  nbr 10.0.0.2 aa:bb:cc:dd:ee:ff 11
 *   route <prefix>/<len> <via-ip>              e.g.  route 192.168.5.0/24 10.0.0.2
 * `nbr` installs a /32 host route to the neighbor via the interface on <via-vlan>.
 * `route` installs an LPM route whose next hop is the (already-defined) neighbor
 * <via-ip>. Returns 0 on success, <0 on parse/program error.
 */
int l3_config_load(int unit, const char *path);

#endif /* EDGED_L3_H */
