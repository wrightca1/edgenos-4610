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

So the **control plane is complete**. The remaining piece for **hardware**
forwarding is the bcmd netlink→chip L3 sync (RTM_NEWROUTE → `bcm_l3_route_add`):
today the OSPF routes are in the Linux FIB (CPU/slow-path); the netlink sync will
mirror them into the ASIC L3 tables so the chip forwards them at line rate.

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
