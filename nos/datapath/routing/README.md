# Routing daemons (Quagga OSPF/BGP) for EdgeNOS-4610

Open-source routing control plane for the Edgecore AS4610-54T — **Quagga 1.2.4**
(`zebra` + `ospfd` + `bgpd`), cross-compiled for armhf. Runs on the per-port
Linux netdevs that `bcmd` creates (`ge0..ge47`, `xe0..xe5`).

## Status: OSPF VERIFIED WORKING (2026-06-10)

On the live box, with `bcmd` up and `ge25` on 10.14.1.0/24:
- `ospfd` formed a **FULL OSPF adjacency** with the upstream router 10.14.1.254.
- **OSPF hellos (224.0.0.5) are punted to the CPU** — the chip floods link-local
  multicast to the CPU and the KNET ingress filter delivers it to the port netdev
  (verified: mDNS 224.0.0.251 + the OSPF hellos both arrive on `ge25`).
- `zebra` installed the learned routes into the Linux kernel FIB:
  `default` + 10.3/10.4/10.11/10.12/10.20/10.21/10.100.1.0/24 `via 10.14.1.254 proto zebra`.

So the **control plane is complete**.

## netlink→chip L3 sync: DONE + verified (2026-06-10)

`bcmd` now mirrors the Linux FIB into the ASIC L3 tables, so routes the kernel
learns (static **or** OSPF/BGP) are forwarded by the chip at line rate. The sync
is a `NETLINK_ROUTE` listener + periodic FIB re-dump:

| netlink event | chip action | bcmd log line |
|---|---|---|
| `RTM_NEWLINK` (admin up/down) | `bcm_port_enable_set` | `netlink: geN -> chip port admin UP/DOWN` |
| `RTM_NEWADDR` (switch's own IP) | L3 intf + MY_STATION + **`BCM_L3_L2TOCPU` /32** | `L3: intf …` / `L3: local … -> CPU (HW punt)` |
| `RTM_NEWNEIGH` (ARP entry) | `bcm_l3_egress_create` + `bcm_l3_host_add` | `L3: host A.B.C.D -> egr … (HW)` |
| `RTM_NEWROUTE` (via gateway) | `bcm_l3_route_add` (→ gateway egress) | `L3: route P/len -> egr … (HW)` |

Verified on the live box: `ip route add 192.0.2.0/24 via 10.14.1.254 dev ge25`
→ `L3: host 10.14.1.254 -> egr 100002` + `L3: route 192.0.2.0/24 -> egr 100002`,
ping to the gateway stays 0% loss. OSPF-learned routes use the identical
`RTM_NEWROUTE` path, so they program the same way once they hit the FIB.

Three fixes were required (see git log / memory):
1. **`bcmSwitchL3EgressMode=1`** — object-based `bcm_l3_egress_create` returns
   `BCM_E_DISABLED` (-12) until egress mode is on. Set once before the first egress.
2. **Local-IP CPU punt** — MY_STATION makes the chip L3-route any frame to the
   router MAC, so the switch's *own* inbound unicast (ICMP/OSPF replies) was
   dropped on L3-miss until a `BCM_L3_L2TOCPU` /32 host entry was added per local IP.
3. **id-0 sentinel** — L3 intf / egress id `0` is valid; cache + callers must use
   `-1` (not falsy `0`) as the "none" sentinel, else the first interface is rejected.
   Also: netlink dumps must be drained one-at-a-time (`NLMSG_DONE`) or the 2nd/3rd
   dump gets `EBUSY` and the neigh/route FIB is never read.

### OSPF adjacency note (far-side blocker)
Our OSPF side is fully correct: hellos egress, we reach **2-Way** (each router
lists the other as a neighbor, params/area/MTU all match), and we send DBDs as
Master. But the upstream **DR (router-id 10.101.1.241 / 10.14.1.254) never sends a
DBD back** — it stays at 2-Way and won't form the adjacency, even after a full
dead-timer (40 s) flush + clean restart. That's a *far-side* condition (likely an
OSPF MTU mismatch or network-type/config issue on 10.101.1.241), not ours. When it
converges, `bcmd` will program the routes automatically (mechanism proven above).

## Build
```sh
# one-time: clone the source (savannah is unreachable; use the GitHub mirror)
git clone --depth 1 --branch quagga-1.2.4 https://github.com/Quagga/quagga.git dl/quagga-git
./build-quagga.sh        # -> routing/{zebra,ospfd,bgpd}-arm (armhf)
```
Cross-builds in the `edgenos/builder9` docker (has autoconf/automake/gawk +
arm-linux-gnueabihf), bootstraps the git tree, stubs `crypt()` (no static
libcrypt), `-fcommon` for GCC10+. Binaries are dynamically linked against ONL's
armhf glibc (they run as-is on the box).

## Run (on the box)
```sh
mkdir -p /etc/quagga /var/run/quagga /usr/local/var/run/quagga
# zebra.conf:  hostname/password
# ospfd.conf:  router ospf / ospf router-id <ip> / network <subnet> area 0
zebra-arm -d -f /etc/quagga/zebra.conf -i /var/run/quagga/zebra.pid
ospfd-arm -d -f /etc/quagga/ospfd.conf -i /var/run/quagga/ospfd.pid
# inspect: telnet localhost 2601 (zebra) / 2604 (ospfd) / 2605 (bgpd), pw "zebra"
ip route show proto zebra        # OSPF/BGP-learned routes in the FIB
```

`bgpd-arm` is built and runs (`--version` ok); BGP config is the standard Quagga
`router bgp <asn>` / `neighbor` syntax. Not yet peered in testing.

> FRR (the modern Quagga successor) is the eventual target, but its libyang/cmake
> static cross-build is painful; Quagga 1.2.4 gives a clean working OSPF/BGP now.
