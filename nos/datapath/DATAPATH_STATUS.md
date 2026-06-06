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
1310nm dual-fiber LR — see `../../live-investigation/dumps/sfp_eeprom_dom_2026_06_06.md`),
the 84758 still reports `sigdet=0` / no 10GBASE-R block-lock.

The 84758 **optical-signal config is now faithful to the real driver**: the
register map was pulled from the open-source robo2-xsdk `phy84740.h`, which fixed
two of our guesses — `OPTICAL_SIG_LVL = 0xc800` (not 0xc8e5) and the level masks
`RXLOS_LVL=b9 / MOD_ABS_LVL=b8`. `sfp_tx_enable()` now sets the c8e4 override **and**
the c800 levels (OpenMDK omits the latter, which had pinned `sigdet=0`).

But that alone doesn't lock the link. Per-MMD diagnosis (`edged --up-check`) on the
plugged ports pinpoints where the 10G chain breaks:

```
xe0/xe1: PMD(1) sd=0 rxlink=0 | PCS(3) blocklock=0 rxlink=0 | PHY-XS(4) st=0000
         cfg-readback 1.0000=2040 1.0007=0008 (10G + 10G-LRM)  c820=e044 (micro up)
```

So the 84758 **is** correctly configured for 10G (speed-select + PMA-type written
and read back; ucode/micro running) and the optics are healthy — but its **media
PMD detects no valid signal** (`sd=0`) so the 10GBASE-R PCS never block-locks.
Also ported from robo2: the 10G PMA `speed_set` (`1.0000=0x2040`, `1.0007` PMA-type
`10G_LRM`) — OpenMDK omits even that.

The 84758-side `phy_84740_init` steps are now ported into `sfp_tx_enable()` from
the open-source driver — all confirmed to take on-box:
- 10G PMA `speed_set`: `1.0000=0x2040` (speed-select), `1.0007`=PMA-type `10G_LRM`.
- **PCS enable**: clear `1.0xcd17` (RESET_CONTROL_REGISTER) — in multi-port
  (4×10G) mode the 84758 holds its media PCS in reset until this is cleared.
- optical override (`c8e4`) + levels (`c800` b8/b9) + TX-enable (the `c800[7]`
  TxOnOff strap).

Yet the media PMD still reads `sd=0` and no register on the 84758 moves it.

**Isolated to the Warpcore SerDes core (2026-06-07).** Reading each PHY in the
chain directly (`edged --up-check`): both the external 84758 **and** the internal
`bcmi_warpcore_xgxs` report `link=0`. Decisively, enabling the **Warpcore's own
internal analog loopback** (TX→RX inside the SerDes — no fiber, no 84758, no
partner) **still yields `link=0`**. So the Warpcore PCS won't lock even on its own
transmit — the 10G datapath failure is the **Warpcore core itself**, not the
optics, fiber, partner, or 84758 (all of which are now verified good / fully
configured).

**Localized precisely (2026-06-07) by reading the Warpcore directly via
`phy_aer_iblk_read`:**
- Warpcore `speed_get = 10000` → it IS configured at 10G (not stuck in 1G).
- `XGXSSTATUS` bit11 `TXPLL_LOCK = 1` → the Warpcore TX PLL is locked.
- BUT `PCS_IEEESTATUS1` bit2 `RX_LINKSTATUS = 0` even with the Warpcore's **own
  internal analog loopback** engaged (TX→RX inside the SerDes — no fiber/84758/
  partner). (The `pd_link_get`-reports-`link=1` in loopback is the *1G COMBO*
  status, not the 10G PCS — a red herring.)

So the failure is exactly the **Warpcore 10G RX PCS/datapath not locking even on a
pristine looped-back signal**, despite correct speed + PLL lock. This is the
**OpenMDK Warpcore RX-datapath/adaptation gap** (same root as the 20G/QSFP work in
`project_qsfp_persist_and_eq` / `project_wc_fw_pause_protocol`; OpenMDK lacks the
full SDK's per-lane RX bring-up / `independent_lane_init`). It is **not** reachable
by register config from `edged`. Everything up to it — copper, L2, SFP+ MAC@10G,
84758 (speed/PCS/optical/laser per the open-source driver), modules/optics, the
Warpcore PLL+speed — is done and verified. Diagnostic tooling (per-PHY link,
Warpcore PLL/PCS reads, internal-loopback isolation) is in `edged --up-check`. The
realistic path to close it is the **full Broadcom SDK datapath build** (OpenBCM/
OpenNSL), which carries the Warpcore RX bring-up OpenMDK omits.

## OpenMDK changes

See [`openmdk-patches/`](openmdk-patches/) — 4 patches against canonical OpenMDK
`PKG/` source, plus the BCM84758 driver+firmware (Broadcom source-available, in
`phy84758-src/broadcom-official/`; `apply.sh` installs the ucode). Build flags
`-DPHY_CONFIG_INCLUDE_BCM84740=1 -DPHY_CONFIG_INCLUDE_BCM54282=1` are in
`build-datapath.sh`.
