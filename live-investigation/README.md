# AS4610-54T — Live Stock-NOS Investigation

Dynamic analysis of a **live AS4610-54T running the factory Edgecore NOS**
(Broadcom FASTPATH/ICOS). This is the 4610 analogue of the 5610's Cumulus
baseline mining: a working reference NOS on our **exact BCM56340 "Helix4"**
silicon, captured read-only. Everything here informs `../nos/` (our own NOS).

> All data gathered **read-only** over the serial console (and one non-persistent
> `serviceport dhcp` to get a mgmt IP). No config was saved; nothing was written
> to the chip. Raw captures are in [`dumps/`](dumps/); the driver is
> [`tools/sercon.py`](tools/sercon.py).

---

## The box

| | |
|---|---|
| Hardware | Accton/Edgecore **AS4610-54T**, S/N EC2025000934, MAC 04:F8:F8:15:A8:41 |
| NOS | **Broadcom FASTPATH / ICOS** `3.4.3.7` (`FastPath-ICOS-esw-as4610-iproc`, Feb 2020) |
| Kernel | **Linux 4.4.39-Broadcom XLDK-4.2.1**, armv7l, SMP PREEMPT |
| U-Boot | 2012.10 (Apr 2017), on SPI-NOR |
| NPU | **BCM56340_A0 rev 0x01** (Helix4) — *our target chip* |
| Packages | BGP-4, QOS, Multicast, IPv6, Routing, Data Center, **OpEN API** |
| Mgmt | serial `ttyS0` 115200 8N1; OOB `eth0` (serviceport) DHCP → **10.1.1.200** |

## How to get in (reproducible)

Serial `/dev/ttyUSB1` @ 115200. Login **`admin`** / **(blank password)**.
The CLI is FASTPATH (`(Routing) >`). Then:

- `enable` → privileged `(Routing) #`; `terminal length 0` disables paging.
- **`linuxsh`** → root Linux shell (a localhost telnet to `utelnetd` on :2324,
  login program `/bin/sh`). Full busybox userland, `uid=0`.
- **`bcmsh`** → Broadcom diag shell (`BCM.0>`). ⚠️ *blocks other management while
  active*; `exit` restores it. Read-only `ps`/`soc`/`config show` only.

`tools/sercon.py` (pyserial) drives all of this one command at a time and
captures output (`send`/`read`/`key`/`raw` modes).

---

## How the stock NOS works (synthesis)

### Boot / init chain  — [`dumps/rc.fastpath`](dumps/rc.fastpath), [`dumps/rc.start`](dumps/rc.start)

```
U-Boot (SPI-NOR) → kernel + ramdisk (root=dev/ram)
  → /etc/rc.d/rc.fastpath  (sets CONFIG_DEV=/dev/sda2, sources rc.init, rc.start)
    → /etc/rc.d/rc.start   (platform init; startup menu; run_application())
       - mknod /dev/linux-kernel-bde (c 127 0), /dev/linux-user-bde (c 126 0)
       - insmod /lib/modules/*.ko  (BDE modules, with INSMOD_PARAMS)
       - procmgr → supervises the app set
```

Running rootfs is a **RAM disk**; the persistent OS images live on disk
(`/mnt/fastpath/image1`,`image2`), and the live runtime is unpacked into
**`/mnt/application`** (tmpfs).

### Process model — [`dumps/linux_ps.txt`](dumps/linux_ps.txt)

`procmgr` supervises:
- **`switchdrvr boot`** — the SDK + datapath engine (≈756 MB RSS). The core.
- `syncdb` — state DB. `sock_agent_app` — IPC.
- Routing/feature apps: `ospf_app`, `vr_agent_app`, `vrf_init_app`, `ping_app`,
  `traceroute_app`, `restconf_app`.
- Management: `lighttpd` + `open_magnet` (OpEN API) + `restconf_magnet`,
  `netsnmp_app`, `opensshd` (NETCONF/830 — so TCP/22 is closed), `utelnetd`.

Everything ships under `/mnt/application/` (binaries, `*.so`, OpEN API,
`devshell_symbols.gz`, lighttpd/snmp/sshd configs) — see
[`dumps/linux_fsmap.txt`](dumps/linux_fsmap.txt).

### Chip access / datapath — [`dumps/linux_lsmod.txt`](dumps/linux_lsmod.txt), [`dumps/bcm_soc.txt`](dumps/bcm_soc.txt)

The production model — **directly relevant to our `nos/datapath/` "Path B"**:

- **Kernel BDE**: `linux_kernel_bde` + `linux_user_bde` (proprietary, out-of-tree)
  bound to the on-die CMIC. Device nodes `/dev/linux-kernel-bde` (maj 127),
  `/dev/linux-user-bde` (maj 126). (vs. our "Path A" `/dev/mem` — this is the
  full DMA/IRQ path we'd build for production.)
- **CMIC**: mapped at kernel VA `0xa698e000` (phys `0x48000000`, as we found in
  the DT). Chip `BCM56340_A0`, `attached initialized mem-clear-use-dma`.
- **DMA**: `PKT DMA dcb=t23` → Helix4 uses **DCB type 23** (the 5610/BCM56846 was
  type 21 — new data point). Channels: **ch0 = TX, ch1 = RX** (active), DV ring
  `max-q=32`. Table programming via **SCHAN** (7.4M ops) + Table/TSLAM DMA.

### Live port map — [`dumps/bcm_ps.txt`](dumps/bcm_ps.txt)

Authoritative SDK enumeration of the BCM56340 on this board:

| Logical | Count | Type | Front panel |
|---|---|---|---|
| `ge0`–`ge47` | 48 | 1G **SGMII** (external PHYs) | 48× RJ-45 |
| `xe0`–`xe3` | 4 | 10G **XGMII** | SFP+ 49–52 |
| `xe4`–`xe5` | 2 | 20G XGMII | QSFP+ 53–54 |
| `ge48` | 1 | 1G **GMII**, jumbo 16356 | CPU port |

### Chip config (the `config.bcm`) — [`dumps/bcm_config_show.txt`](dumps/bcm_config_show.txt)

151 live SDK properties, incl. the **per-port external-PHY MDIO addresses**
(`port_phy_addr_ge0..47` = 0x5..0x3b), lane mapping
(`phy_port_primary_and_offset_geN`), `phy_fiber_pref_xe0..3=1`,
`phy_sgmii_autoneg_ge=1`, `bcm_num_cos=8`, `lpm_scaling_enable=1`. This is the
property set to bring this exact board's datapath up.

### Storage / partition layout — [`dumps/storage_layout.txt`](dumps/storage_layout.txt)

For the **backup plan** (next phase):

| Device | Size | Role |
|---|---|---|
| `/dev/sda` | ~30.9 GB | **USB flash** (primary; "mounted first as /dev/sda") |
| `sda1` | 256 MB | (boot/loader area) |
| `sda2` | 512 MB | `ext2` → `/mnt/fastpath` (images, config, logs, ssh keys) |
| `/dev/sdb` (sdb1/sdb2) | TBD | alternative config device (`CONFIG_DEV_ALTERNATIVE=/dev/sdb2`) |
| SPI-NOR (`mtd0/1/2`) | 896k/64k/64k | `u-boot` / `shmoo` (DDR cal) / `env` |

Most of the 30 GB is unpartitioned — a backup needs the **partition table +
sda1 + sda2 + sdb + the SPI MTDs**, not the full 30 GB. Tools on the ramdisk:
`dd`, `tar`, `gzip`, `tftp`, `wget`, `ftpput` (no `nc`/`scp`/`ssh`). The cleaner
imaging environment is **ONIE** (partitions unmounted; better tooling).

---

## What this gives our NOS (`../nos/`)

- **Confirms Path B**: the production driver is `linux-kernel-bde`/`linux-user-bde`
  on the CMIC at phys `0x48000000` — exactly what `nos/datapath/BDE_ACCESS.md`
  predicted; we now have a live reference of it on BCM56340.
- **DCB type 23 + ch0/ch1 TX/RX + DV-32** — the concrete CMICd DMA parameters for
  our datapath (different from the 5610's type 21).
- **Authoritative port map** (`ge0-47`/`xe0-5`/`ge48`=CPU) and **per-port PHY MDIO
  addresses** + SGMII/fiber properties — drop-in reference for our chip config.
- **A working `config.bcm` and chip-init recipe** to diff our OpenMDK `bmd_init`
  against — the 4610 analogue of the Cumulus rc.soc we mined for the 5610.

---

## Dump index ([`dumps/`](dumps/))

| File | Contents |
|---|---|
| `show_version.txt`, `show_sysinfo.txt` | NOS/platform identity, supported MIBs |
| `show_running-config.txt` | running config (factory default) |
| `show_port_all.txt`, `show_serviceport.txt`, `show_network.txt`, `show_ip_int_brief.txt` | port + mgmt state |
| `help_privileged.txt` | full privileged CLI command list (incl. `bcmsh`/`linuxsh`) |
| `linux_identity.txt`, `linux_eth0.txt`, `linux_net_tools.txt`, `linux_tools.txt` | kernel/uname/cmdline, network, available tools |
| `linux_ps.txt`, `linux_lsmod.txt`, `linux_fsmap.txt` | processes, modules (BDE), filesystem map |
| `storage_layout.txt`, `storage_tools.txt` | partitions/mounts/df, block devices |
| `rc.fastpath`, `rc.start` | boot/init scripts (BDE load + switchdrvr launch) |
| `fastpath.vpd` | image/version VPD + module list |
| `bcm_soc.txt`, `bcm_ps.txt`, `bcm_config_show.txt` | live chip driver state, port map, config.bcm |

## Status / next

- [x] Read-only dynamic analysis of the live ICOS NOS (this doc + dumps).
- [ ] **ONIE partition backup** (disruptive — reboot into ONIE, image
      partitions, transfer off via the network). Plan above; to be done next.
