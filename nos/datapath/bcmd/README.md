# bcmd — EdgeNOS-4610 datapath daemon on the full OpenBCM SDK

`bcmd` is the data-plane daemon for the **Edgecore AS4610-54T** (BCM56340
"Helix4") built on the **full OpenBCM SDK (sdk-6.5.16)** BCM C API — the
successor to the OpenMDK-based [`edged`](../mdk-app/edged.c). It is the body of
the daemon, **appended to the SDK's `socdiag.c`** at build time, with the
interactive REPL (`diag_shell();`) diverted to `bcmd_run();`. This reuses
`socdiag`'s `main()` (SAL init, KCOM/BDE/PCI platform hooks) and the proven
`make bcm` recipe — only the REPL is replaced with a deterministic datapath
bring-up.

## Current target: copper port 1 (WORKING — first bidirectional ping)

`BCMD_PORT=26` (ge25 = front-panel port 1, BCM54282) on **10.14.1.0/24**:
**`ping 10.14.1.254` = 0% loss**, ARP resolves, ICMP replies return. The last-mile
fix was the **CPU-punt VLAN-tag strip** — `bcmd_knet_setup` now sets
`filter.flags = BCM_KNET_FILTER_F_STRIP_TAG` so the chip's PVID (VLAN 1) tag is
removed and Linux sees clean untagged frames (without it the kernel saw a bogus
ethertype and never parsed IP/ARP). `BCMD_PORT` is **compile-time** — rebuild to
retarget between the copper port (26) and the SFP+ uplink (xe0 = 50).

> **Loop warning:** front port 1 (ge25) must be on the **10.14.1.0/24 VLAN, not the
> 10.1.1 mgmt LAN** — ge25 + mgmt `ma1` on the same 10.1.1 segment is the L2 loop
> that hung the box. Verify with `tcpdump -i port1 -nn` (source IPs must be 10.14.1.x).

The **SFP+ uplink (port 49 / xe0 / 50)** is parked pending replacement optics; its
blocker is an optical-overdrive issue on the link, proven *not* software via an
ICOS A/B test (see below).

## Why the full SDK

OpenMDK proved the hardware works (copper links, MACs forward), but its
bcm56340 BMD lacks the CPU-punt DMA path and the PHY/serdes link-up machinery.
The full SDK gives us the production `phy84740` PHY driver, KNET, and a complete
`bcm_port`/linkscan flow. `bcmd` drives it deterministically as a daemon.

## What `bcmd_run()` does, in order

1. **`bcmd_deassert_ext_phy_reset()`** — clear the board CPLD reset (see below).
   **Must run before `init all`.**
2. **`bcmd_mdio_to_cmic()`** — hand the external MDIO to the CMIC (clear iProc
   MDIO-sel `0x1803fc3c` bit4).
3. **`bcmd_write_icos_miim_map()`** — replay ICOS's CMIC MIIM bus-map regs
   (`0x48011000`, 22 registers).
4. **`bcmd_chip_init()`** — `sysconf_init/probe/attach` + `init all` + `init bcm`.
   With the PHYs now out of reset, the SDK probes and attaches the 84758
   (`phy84740` driver, firmware download, serdes/link bring-up).
5. **`bcmd_port_up(50)`** — SW linkscan + `bcm_port_enable` + pause off.
6. **`bcmd_sfp_laser_all()`** — **read-only** 84758 status now (the SDK owns the
   PHY datapath; the old OpenMDK-era manual register surgery is `#if 0`).
7. **`bcmd_rx_start()`** — `bcm_rx_start` with `cos_bmp = all` (the CPU-punt DMA fix).
8. **`bcmd_knet_setup()`** — KNET netif `uplink` (port 50) + catch-all filter.
9. **`bcmd_l2_punt()`** — static our-MAC→CPU L2 entry.
10. Wait loop, logging link + chip RX/TX counters + 84758 PCS every ~10 s.

## THE root cause this resolved: external PHYs held in CPLD reset

EdgeNOS/ONL boots with the external front-panel PHYs (84758 10G SFP+ **and**
54282 1G copper) **held in reset by CPLD GPIOs**: `CPLD[0x19]=0x7f`,
`CPLD[0x1b]=0xef` (i2c-0 @ 0x30, the `accton_as4610_cpld`). The SDK's `init all`
programs the CMIC MIIM master correctly but **cannot clear a board-level CPLD
reset**, so every MIIM read of an external PHY returns `0xffff`. ICOS clears
`0x19`/`0x1b`=`0x00` at boot. `bcmd` (and `bringup-bcmd.sh`) do the same with
`i2cset` **before `init all`**, so the SDK's PHY probe finds the now-awake 84758.

> The earlier "CMIC `OVER_RIDE_EXT_MDIO_MSTR_CNTRL` register" hypothesis was a
> red herring — those CMIC regs already matched ICOS (the SDK sets them). The
> CPLD reset was the missing piece. The in-process raw-i2c CPLD write is
> best-effort (the bound CPLD driver wants SMBus-byte-data semantics); the
> reliable deassert lives in `bringup-bcmd.sh` via `i2cset -f`.

After the fix the 84758 is reachable at MIIM `phy_id 0x40-0x43` (`600d:86f0`),
firmware loads (`1.0xca1c=0x600d`, rom ver `0x128`), laser on (`c800=0x3f3f`),
and **port 50 links at 10G with PCS block-lock**.

## SFP+ uplink (port 49) — the remaining issue is NOT software

With the link up at 10G, our **TX works** (the upstream Cumulus box receives our
ARP and has our MAC in `ip neigh`), but **chip RX = 0** and ping fails. Extensive
diagnosis (PHY loopback passes 20/20 frames; firmware loaded; 84758 config
byte-identical to ICOS; chip Warpcore RX/XMAC proven good) isolated it to the
**optical link being overdriven**:

- Far-side Cumulus `swp4` optic DOM: **RX power +2.96 dBm → RX-POWER-HIGH alarm**.
- Our AS4610 RX: −1.2 dBm (borderline).
- Both 1310 nm 10G-LR optics on a short/zero-loss fiber → receivers saturate →
  clipped waveform → the 10GBASE-R PCS cannot hold lock → "link up" flaps, RX=0.

**A/B test (definitive):** reflashed the box to stock **ICOS 3.4.3.7** and ran
the same test — `show port 0/49` = **Up 10G**, but `show interface 0/49`
**Packets Received = 0**, ping → *Destination Unreachable*. **ICOS fails
identically.** So the factory NOS + full Broadcom SDK also cannot pass traffic
over this link → it is the **optical overdrive**, not EdgeNOS. This vindicates
the entire open-source stack as functionally equivalent to ICOS on this board.

**Fix is physical** (no software knob — LR optical TX power is set by the
module's internal APC laser loop; the RX saturates at the optical front-end
before any host-adjustable gain): add a **~5 dB inline optical attenuator**
(or a lossier/longer fiber, or SR optics/DAC for a short link).

## Build / deploy / run

```sh
# build (docker builder, SDK mounted at /sdk — paths are baked into cached libs):
./build-bcmd.sh                      # -> bcmd (armhf ELF), staged here + in ../artifacts/

# deploy to the switch (persistent /mnt/onl/data):
scp bcmd bringup-bcmd.sh root@<sw>:/mnt/onl/data/
scp ../artifacts/linux-{kernel,user}-bde.ko ../artifacts/linux-bcm-knet.ko root@<sw>:/mnt/onl/data/
scp ../config.bcm.as4610-54t root@<sw>:/mnt/onl/data/config.bcm

# bring up from a fresh boot (unbinds iproc_cmic, loads BDE+KNET, deasserts CPLD,
# launches bcmd):
ssh root@<sw> 'cd /mnt/onl/data && sh bringup-bcmd.sh'
# log: /tmp/bcmd.log ; netdev: ip link show uplink
```

## Diag shell for PHY/serdes debugging

`build-diag.sh` builds the **stock** SDK `bcm.user` (same recipe, no `bcmd_run`
diversion) — the full interactive diag shell. `run-diag.sh` deasserts the CPLD +
writes the bus-map, then drives `bcm.user` with diagnostic commands (`ps`,
`phy xe0`, `phy diag xe0 dsc`, `port xe0 lb=...`, `show c`). This is how the
PHY-loopback, firmware-checksum, and 84758-register-diff evidence above was
gathered.

## Files

| File | What |
|---|---|
| `bcmd.c` | daemon body (appended to `socdiag.c`) |
| `build-bcmd.sh` | build `bcmd` from the SDK (docker) |
| `bringup-bcmd.sh` | from-scratch bring-up on the switch (CPLD deassert + modules + launch) |
| `build-diag.sh` | build the stock SDK diag shell `bcm.user` |
| `run-diag.sh` | drive the diag shell for PHY/serdes investigation |

See also: [`../DATAPATH_STATUS.md`](../DATAPATH_STATUS.md),
[`../mdk-app/edged.c`](../mdk-app/edged.c) (the OpenMDK predecessor),
`../../live-investigation/RESTORE.md` (ICOS backup/restore — used for the A/B test).
