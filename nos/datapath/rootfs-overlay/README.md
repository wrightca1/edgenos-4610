# EdgeNOS-4610 config-file scheme (mirrors the AS5610)

Config-file-driven, persistent datapath + control-plane bring-up for the AS4610,
modeled on the AS5610 (`newnos/config/rootfs/overlay/`). Everything that defines
the running config lives in editable files — no hard-coded addresses.

## Files
| File | Purpose |
|---|---|
| `etc/edged/addrs.conf`   | per-port L3 addresses (`<iface> <cidr> [mtu]`) — ge25=10.14.1.2/24, xe0=10.101.102.2/29, MTU 1600 |
| `etc/edged/routes.conf`  | static transit routes (empty by default; OSPF learns the rest) |
| `etc/quagga/zebra.conf`  | Quagga zebra (FIB manager) |
| `etc/quagga/ospfd.conf`  | OSPF: router-id 10.14.1.2, networks 10.14.1.0/24 + 10.101.102.0/29 |
| `usr/sbin/bcmd-prep.sh`  | platform prep (unbind CMIC, load BDE+KNET `default_mtu=1600`, devnodes, CPLD reset) |
| `usr/sbin/edgenos-l3-config.sh` | apply addrs.conf + routes.conf after bcmd is ready (bcmd's netlink sync mirrors them into the chip) |
| `etc/systemd/system/{bcmd,edgenos-l3,zebra,ospfd}.service` | boot ordering (≈ 5610 edged/swp-l3/zebra/ospfd) |

## Two deployment modes

**1. Baked into a custom EdgeNOS-4610 SWI (the proper, persistent way).** Copy
`rootfs-overlay/` into the image rootfs and `systemctl enable bcmd edgenos-l3
zebra ospfd`. Then config + services survive reboot exactly like the 5610. (The
datapath binaries — bcmd, the 3 .ko, config.bcm — live in `/mnt/onl/data`, which
the services reference; only the control-plane + confs are baked.)

**2. Stop-gap on stock ONL (no image rebuild yet).** Stock ONL resets `/etc` and
`/usr` every boot (the root overlay's upper layer is initramfs tmpfs) and has no
built-in persistent boot hook — only `/mnt/onl/{config,data}` persist. So stage
this whole tree under `/mnt/onl/data/edgenos/` and run `edgenos-up.sh` (which
reads the same config files via `EDGENOS_ETC`). The **files persist**; auto-run on
boot needs either mode 1 or a manual `sh /mnt/onl/data/edgenos/edgenos-up.sh`.

## MTU note
The peer network runs MTU 1600 — OSPF will not leave ExStart on a DBD-MTU
mismatch, so KNET is loaded `default_mtu=1600` and addrs.conf lists 1600. bcmd
also raises the chip frame-max to jumbo so 1600-byte data forwards.
