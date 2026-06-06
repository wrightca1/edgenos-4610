# EdgeNOS-4610 datapath — bring-up status

`edged` (`mdk-app/edged.c`) is the open-source data-plane daemon for the
**Edgecore AS4610-54T** (BCM56340 "Helix4" SoC). It maps the on-die CMIC at phys
`0x48000000` via `/dev/mem`, attaches the OpenMDK `bcm56340_a0` BMD, brings the
chip up, binds the front-panel PHYs, and forwards L2. This file is the running
status of what works and what's left.

Build: `./build-datapath.sh` (cross arm-linux-gnueabihf in the builder image).
Run on the switch: `/tmp/edged` (one-shot init) or `/tmp/edged --keep` (resident,
polls link). Diagnostics: `--scan-link`, `--copper-up`, `--up-check`, `--scan-mdio`.

## Status at a glance

| Plane | State |
|---|---|
| Chip init (reset/init/swinit) | ✅ working |
| **48× 1G copper (BCM54282)** | ✅ **links** — jack-to-jack cable comes up at 1G |
| L2 forwarding (VLAN 1, STP) | ✅ 55/55 ports forwarding |
| **4× 10G SFP+ MAC (BCM84758)** | ✅ **ports configured at 10G** (`inventory 4×10G`), PHYs bound, ucode loaded, lasers on |
| 4× 10G SFP+ optical link (PCS lock) | ⏳ not yet — Warpcore 10G SerDes / 84758 media-RX (see below) |
| 2× QSFP (xe4-5, Warpcore) | not yet attempted on this board |

## How the front-panel PHYs are reached (the hard part)

The 54282 (1G copper) and 84758 (10G SFP+) hang off the **CMIC MIIM**, but the
stock OpenMDK bcm56340 BMD does not reach them as-shipped. Three things were
missing, all now handled by `edged` + the OpenMDK patches:

1. **External PHY reset** — ONL leaves CPLD `0x19=0x7f`/`0x1b=0xb5` (PHYs held in
   reset). `edged` clears them over i2c (`deassert_ext_phy_reset`, SMBus + retry).
2. **CMIC MIIM bus map** — `0x48011000` must be programmed (ICOS values, replayed
   by `write_icos_miim_map`) or the external buses are dead. It must be live
   *before* the probe, so `edged` writes it after `bmd_reset` (which clears it).
3. **Correct port→MDIO map** — `bcm956340k_miim_ext.c` `_phy_addr()` rewritten to
   the real board layout (patch 03): copper jacks 1-24 → EBUS0 `0x01-0x1c`,
   25-48 → EBUS1, SFP+ → EBUS2 `0x80-0x83`.

Because `bmd_init` runs the PHY probe *before* the bus map is live, the external
copper PHYs first bind the generic `unknown` driver; `edged` **re-probes** ports
1-48 after init (bus map now live) so the real `bcm54282` driver binds and inits
the copper media. After that, a jack-to-jack cable links at 1G.

## SFP+ 10G — what was solved

The AS4610 is 48×1G + 4×10G, but OpenMDK's static bcm56340 port configs don't
offer that combo (cfg 400 caps the SFP+ at 2.5G; the "4x10" cfg 410 is really
1×20G; cfg 497 is 4×10G but drops 8 copper ports). Solved with two surgical
OpenMDK edits (patches 01 + 02):

- `port_speed_max_400[]`: the 4 SFP+ entries `2.5G → 10G` (only those 4 change).
- `bmd_switching_init`: per-port config failure made non-fatal, so the SFP+ 10G
  config (TDM still nominally 2.5G) can't abort L2.

Result on a default boot: `inventory 4×10G`, `L2 forward 55/55`, copper still
links, `bcm84740`+Warpcore bound on xe0-3, 84758 ucode loaded
(`fw-checksum 1.0xca1c = 0x600d`).

### SFP+ optical TX (lasers) — solved

OpenMDK's bcm84740 driver never deasserts the SFP `TX_DISABLE`. The 84758 drives
that pin via its own GPIO; `c8e4[4] = TxOnOff_strap XOR c800[7]`, and the board
straps it so the laser starts **off**. `edged`'s `sfp_tx_enable()` ports the
robo2 enable sequence: clear PMD `1.0009`/`1.0xcd16`, set the optical-cfg
overrides, and **toggle `1.0xc800[7]`** to flip `c8e4[4]→0` (TX active). Verified:
module `TX_DISABLE` deasserts and `module_rx_los_all` goes `0x03→0x00` — both SFPs
see light.

## What's left: the optical PCS lock

With the MAC at 10G, lasers on, light flowing, and compatible modules (both
1310nm LC), the 84758 still reports `sigdet=0` / no 10GBASE-R block-lock. This is
the **Warpcore 10G SerDes lock / 84758 media-RX** layer — the same per-lane
RX-calibration gap documented for the 40G QSFP work (OpenMDK lacks the full SDK's
`independent_lane_init`). It is no longer a port-config problem; it's the SerDes
physical-layer lock, and is the next focused effort.

## OpenMDK changes

See [`openmdk-patches/`](openmdk-patches/) — 4 patches against canonical OpenMDK
`PKG/` source, plus the BCM84758 firmware note (kept local). Build flags
`-DPHY_CONFIG_INCLUDE_BCM84740=1 -DPHY_CONFIG_INCLUDE_BCM54282=1` are in
`build-datapath.sh`.
