# AS4610-54T — SFP+ 10G / Warpcore bring-up investigation (how we got here)

This is the detailed, chronological record of bringing the AS4610-54T data plane up
with our open-source `edged` daemon, ending at the **one remaining blocker** — the
Warpcore 10G SerDes RX won't lock — and the **plan to crack it by dynamic analysis
of the stock ICOS NOS** (which has the full Broadcom SDK and *does* lock the link).

Written deliberately **before** we reflash the box to ICOS, so the full context and
the *unlocked* baseline survive the wipe. Companion docs:
[`DATAPATH_STATUS.md`](DATAPATH_STATUS.md) (terse status table),
[`openmdk-patches/`](openmdk-patches/) (the source changes),
[`../../live-investigation/RESTORE.md`](../../live-investigation/RESTORE.md) (ICOS
restore), and the ICOS capture plan at the end of this file.

---

## 0. Objective & hardware

Bring up a **fully open-source** data plane on the **Edgecore AS4610-54T**:
- SoC: Broadcom **BCM56340 "Helix4"** — dual ARM Cortex-A9 + switch fabric + SerDes
  on one die; CPU↔switch is **on-die CMIC** at phys `0x48000000` (no PCIe/PAXB,
  unlike the 5610). Reached via `/dev/mem` (STRICT_DEVMEM off).
- Front panel: **48× 10/100/1000BASE-T** (RJ-45) + **4× 10G SFP+** (xe0-3) + 2× 20G
  QSFP stacking (xe4-5, not attempted).
- PHYs: **BCM54282** octal 1G copper QSGMII (48 ports, on CMIC MIIM EBUS0/1);
  **BCM84758** 10G SFP+ PHY (84740-family, EBUS2); **Warpcore (WC40)** internal
  10G/20G SerDes (the chip's own MAC-side SerDes).
- CPLD: **Lattice MachXO2 `LCMXO2-1200HC`** (confirmed from board photo), driven over
  i2c as `accton_as4610_cpld`. Board `ES4654BH-0917-EC (4610-54T-O-AC-Fv1) MAIN`,
  S/N `EC2025000934`.

Daemon: `edged` (`mdk-app/edged.c`, BSD-3-Clause) links OpenMDK's source-available
CDK/BMD/PHY. Build: `./build-datapath.sh`. No proprietary kernel module.

---

## 1. Reaching the front-panel PHYs (the access problem)

The stock OpenMDK `bcm56340_a0` BMD does **not** reach the external PHYs as-shipped.
Three things were missing; all are now handled by `edged` + 4 OpenMDK patches:

1. **External PHY reset (CPLD).** ONL leaves CPLD `0x19=0x7f` / `0x1b=0xb5` (PHYs held
   in reset). `edged`'s `deassert_ext_phy_reset()` clears them over i2c. Early raw
   `write()` raced the bound `accton_as4610_cpld` driver and failed silently → rewrote
   to **SMBus byte-data ioctls with 8× retry + 3 ms delay + read-back verify**. Now
   reliable from cold-boot defaults.
2. **CMIC MIIM bus map.** `0x48011000` must be programmed (the ICOS values, replayed by
   `write_icos_miim_map()`) or the external MDIO buses are dead. `bmd_reset` clears it,
   so `edged` writes it **after** `bmd_reset` and **before** the probe.
3. **Correct port→MDIO map.** `bcm956340k_miim_ext.c` `_phy_addr()` rewritten (patch 03)
   to the real board layout: copper jacks 1-24 → EBUS0 `0x01-0x1c`, 25-48 → EBUS1
   `0x01-0x1c`, SFP+ xe0-3 → EBUS2 `0x80-0x83`. The stock gap-of-1 formula didn't match
   the board (gap-of-2 + bus split), so nothing bound.

Because `bmd_init` runs the PHY probe **before** the bus map is live, `edged` **re-probes**
ports 1-48 after init so the real `bcm54282` driver binds (it first binds `unknown`).

### Driver-binding gotchas solved
- `-DPHY_CONFIG_INCLUDE_BCM54282=1` / `-DPHY_CONFIG_INCLUDE_BCM84740=1` build flags
  (driver wasn't in the probe list).
- Stale build cache: `bmd_phy_drv_list.o` not recompiled, `libbmdshared.a` "up to date" —
  had to touch source + `rm` the `.a`.

**Result:** all 48 copper PHYs answer (`id=600d:845b`); a jack-to-jack cable links at 1G;
L2 forwards 55/55 (VLAN 1, STP FORWARDING).

---

## 2. SFP+ MAC at 10G (chip-side port config)

OpenMDK's static `bcm56340` port configs don't offer 48×1G + 4×10G (cfg 400 caps SFP+
at 2.5G; cfg 410 "4x10" is really 1×20G flex; cfg 497 is 4×10G but drops 8 copper).
Solved with two surgical OpenMDK edits:
- **Patch 01** `bcm56340_a0_bmd_attach.c`: `port_speed_max_400[]` — the 4 SFP+ entries
  `3` (2.5G) → `10` (10G). Only those 4 change; all 48 copper identical.
- **Patch 02** `bcm56340_a0_bmd_switching_init.c`: a single port's `_config_port`
  failure made **non-fatal** (don't poison `rv`/abort the loop), so the SFP+ 10G config
  can't take L2/STP down with it (was `0/55`).

A dynamic-config approach (`--try-10g`, `edged_pcfg[]`) was tried first and **abandoned**
— it disturbed all ports (STP 0/55, `_config_port` errors). The static edit is clean.

**Result:** `inventory 4×10G`, `L2 forward 55/55`, copper still links, `bcm84740`+Warpcore
bound on xe0-3.

---

## 3. BCM84758 PHY — claim, ucode, optical/laser (the open-source robo2 port)

- **Patch 04** `bcm84740_drv.c`: teach the 84740 driver to **claim the 84758** (ID
  `0x600d:0x86f0`) and download `bcm84758_ucode` in `init_stage_3`. Verified on box:
  `fw-checksum (1.0xca1c) = 0x600d` — **ucode loaded**.
- The **84758 register map was pulled from the open-source robo2-xsdk `phy84740.h`**
  (Broadcom's own public GitHub, source-available `Legal/LICENSE`; committed under
  `phy84758-src/broadcom-official/`). This fixed two earlier *guesses*:
  `OPTICAL_SIG_LVL = 0xc800` (not 0xc8e5) and level masks `RXLOS_LVL=b9 / MOD_ABS_LVL=b8`.
- `edged`'s `sfp_tx_enable()` ports the full robo2 `phy_84740_init` optical sequence:
  - **10G PMA speed_set**: `1.0000 = 0x2040` (speed-select), `1.0007` PMA-type `10G_LRM`
    (`0x8`). OpenMDK omits even this.
  - **PCS enable**: clear `1.0xcd17` (RESET_CONTROL_REGISTER) — in 4×10G multi-port mode
    the 84758 holds its media PCS in reset until this is cleared.
  - **optical override** (`c8e4`) + **levels** (`c800` b8/b9) + **TX-enable**: the laser
    starts off because `c8e4[4] = TxOnOff_strap XOR c800[7]`; we **toggle `1.0xc800[7]`**
    to flip `c8e4[4]→0` (TX active). OpenMDK never deasserts `TX_DISABLE`.

**Result (verified):** module `TX_DISABLE` deasserts; `module_rx_los_all` goes
`0x03→0x00`; both SFPs transmit and receive light. Modules confirmed compatible (both
1310 nm dual-fiber 10GBASE-LR — Finisar `FTLX1370W3BTL` xe0, Delta `LCP-10G3B4QDRME2`
xe1; RX>TX anomaly was per-module DOM miscal, ruled out — see
[`../../live-investigation/dumps/sfp_eeprom_dom_2026_06_06.md`](../../live-investigation/dumps/sfp_eeprom_dom_2026_06_06.md)).

---

## 4. The blocker: Warpcore 10G RX won't lock — localized precisely

With MAC@10G, ucode loaded, lasers on, light flowing, compatible modules — the 10G link
still won't come up. Localized with `edged --up-check` (per-PHY chain walk + Warpcore
internal-loopback isolation). **Unlocked baseline** (saved verbatim:
[`../../live-investigation/dumps/edged_upcheck_unlocked_baseline_2026_06_06.txt`](../../live-investigation/dumps/edged_upcheck_unlocked_baseline_2026_06_06.txt)):

```
xe0(0x80) cfg[1.0=2040 1.7=0008 c820=e044 spd=10G]
          PMD[st1=0082 st2=b7e1 sd=0000 LA=0]      <- media signal-detect = 0
          PCS[st1=0082 st2=8401 blklk=000c LA=0]   <- 10GBASE-R block-lock = 0
Warpcore-core loopback isolation on port 53 (xe0):
    Warpcore speed_get = 10000                       <- IS at 10G
    pre-lpbk:  XGXSSTATUS=8b08 TXPLL_LOCK=1  PCS_STAT1=0082 RXlink(b2)=0
    in-lpbk:   XGXSSTATUS=8b08 TXPLL_LOCK=1  PCS_STAT1=0082 RXlink=0  drv-link=1
```

Reading each PHY in the chain directly via `phy_aer_iblk_read` (AER/IBLK,
`PHY_REG_ACC_AER_IBLK = 0x50000000`):
- Warpcore `speed_get = 10000` → configured at 10G (not stuck at 1G).
- `XGXSSTATUS` bit11 `TXPLL_LOCK = 1` → TX PLL locked.
- **BUT** `PCS_IEEESTATUS1` bit2 `RX_LINKSTATUS = 0` — even with the Warpcore's **own
  internal analog loopback** engaged (TX→RX inside the SerDes; no fiber/84758/partner).
  (`drv-link=1` in loopback is the *1G COMBO* status — a red herring.)

So the failure is exactly: **the Warpcore 10G RX PCS/datapath won't lock even on a
pristine looped-back signal**, despite correct speed + PLL lock. This is the OpenMDK
**Warpcore uC RX-adaptation / host-cal gap** — the same wall the 40G QSFP effort hit
(`project_wc_fw_pause_protocol`: decoded the `0x820e/0xffe0` fw-pause + host-cal
handshake but didn't fully crack lane adaptation). OpenMDK's `bcmi_warpcore_xgxs` *does*
download WC40 firmware, start the uC, wait for PLL lock, set `RX66_CONTROL` clock-comp
and per-lane `FIRMWARE_MODE` — but the uC-driven RX adaptation never converges to a 10G
PCS lock. It is **not** reachable by register config from `edged`.

### Ruled out: the 5610 "restart-cruft" theory (cold reboot test, 2026-06-06)
The 4610 uses the **same Warpcore family** as the 5610 (both `bcm56340_a0` and
`bcm56840_a0` bind `bcmi_warpcore_xgxs`). The 5610 working-switch effort found a port
that wouldn't link after ~50 `edged` restarts was cured by a **cold boot** (accumulated
lane cruft), lesson: "REBOOT before chasing SerDes." We'd run `edged` dozens of times
this session without a reboot — so we cold-rebooted and ran `edged --up-check` **exactly
once** on a pristine chip. **Result: identical** (`RXlink=0` in internal loopback). So
on the 4610 this is **not** restart cruft — it's a genuine RX-adaptation gap. (The
5610's external DS100DF410 retimers aren't on the 4610 either, so that half of the 5610
SerDes story doesn't transfer.)

### Full-SDK path assessed — not viable with what we have
The local OpenBCM `sdk-6.5.27` tree is **incomplete**: `src/soc/` has only `common/` +
`esw/`, **no `phy/`** — it cannot supply the Warpcore PHY driver / RX bring-up. So
closing the link needs either a **complete** Broadcom SDK with WC40 firmware-cal, or
**reverse-engineering the exact lane-lock register sequence from a NOS that does lock it**
→ which is the next step.

---

## 5. Next step: dynamic analysis of stock ICOS with a LIVE 10G link

**Why now:** the prior `live-investigation/` of ICOS was done **without SFPs** (no link).
Now two compatible SFPs + fiber are in (xe0/xe1, i2c-2/i2c-3, both present). ICOS 3.4.3.7
runs the **full Broadcom SDK**, which performs the Warpcore RX-adaptation — so ICOS should
bring the 10G link **UP**. Capturing the Warpcore lane registers **while locked** and
diffing against our **unlocked baseline** (§4) yields the missing RX-bring-up sequence.
This is exactly the locked-vs-unlocked register-mining that cracked the 5610 (Cumulus).

**This requires a full reflash cycle** (the ONL install repartitioned the disk; original
ICOS `sda1`/`sda2` are gone — see §6). Return path is **secured**: exact running image
pulled to `live-investigation/backup/ONL-edgenos-4610-2026-06-04.swi` (140 MB) + the
`edged` binary + verified ICOS backup.

### Reflash procedure (serial-driven — box is serial-only during this)
Recovery console: `/dev/ttyUSB1` @115200 (auto root shell), helper
`live-investigation/tools/sercmd.py`. Login root/onl (ONL) or admin/blank (ICOS).
1. Boot **ONIE rescue**: interrupt U-Boot, `run onie_rescue` (`tools/catch_uboot.py`).
2. Restore from `backup/` per [`RESTORE.md`](../../live-investigation/RESTORE.md):
   `sgdisk --load-backup=sda.gpt` → `gunzip -c sda1.img.gz|dd of=/dev/sda1` →
   `…sda2…` → `dd if=mtd2_env.bin of=/dev/mtd2`. Reboot → ICOS boots `image1`.
3. Capture (see capture list below) — **one-shot**; the reflash is expensive.
4. Restore edged: ONIE-install `ONL-edgenos-4610-2026-06-04.swi` → boot ONL → re-scp
   `/tmp/edged`. (After any reboot the SWI reverts `ma1` IP + sshd — recover over serial:
   `ip addr add 10.1.1.209/24 dev ma1; ip link set ma1 up`, re-append PermitRootLogin/
   PasswordAuthentication, restart ssh.)

### ICOS capture list (EXPANDED — "capture more")
Do all of these in the ICOS diag shell **with the link UP** (fiber xe0↔xe1):
- **Confirm the link is actually up first**: `show port all` / diag `ps` → expect xe0/xe1
  `up 10G`. (If ICOS *also* can't link, that's a huge finding — means it's the modules/
  board, not our software.)
- **Warpcore locked registers (the gold):** the *same* regs `edged` reads, while locked —
  `XGXSSTATUS (0x8001)`, `PCS_IEEESTATUS1 (0x0001)` — plus the RX-adaptation/DSP block.
  Via diag: `phy diag xe0 dsc` (SerDes eye / DSP / DFE / sigdet), `phy diag xe0 state`,
  `phy diag xe0 regs`, `phy raw`/`phymod` reads of the WC40 lane registers (AER lanes
  0-3). Capture for **xe0 AND xe1**.
- **84758 locked registers:** full MMD dump while linked (PMD `1.x`, PCS `3.x`, AN `7.x`),
  esp. PMD `sd`/`st`, PCS `block-lock`, and the optical-cfg regs (`c8e4`,`c800`,`c820`,
  `cd16`,`cd17`) — to compare with what `edged` writes.
- **Link-up over time:** capture the Warpcore DSC/eye **before** fiber insert and **after**
  it locks (re-seat one fiber while logged in) — the *delta* isolates RX-adaptation
  convergence, controlling for ICOS's static init.
- **The init recipe / firmware:** `phy info` (PHY driver + **firmware version** ICOS loads
  — compare to our v0128 ucode), `config show` / `config.bcm` for the SFP+ ports, SDK
  version, `rc.soc`-equivalent. Any `serdes`/`phymod` firmware-mode + lane config.
- **Broad register baseline (5610-style bulk):** full SOC register dump + relevant chip
  memories while linked, so we can diff anything we didn't think to grab. Save raw.
- **MIIM/SCHAN trace if available:** any diag logging of the PHY register writes during
  link bring-up (the actual missing *sequence*, not just the end state).
- **CPLD state under ICOS:** re-read the CPLD regs (cf. `CPLD_REGISTER_MAP.md`) — confirm
  reset/strap differences vs ONL.

Save everything raw under `live-investigation/dumps/icos_linked_<reg>_2026_06_06.*`.
The diff (ICOS-locked − edged-unlocked, §4 baseline) is the deliverable.

---

## 6. Current on-disk / boot state (why it's a reflash, not a toggle)

The ONL install **repartitioned the whole USB disk**; the original 2-partition ICOS
layout (ACCTON-DIAG + DCSS with `image1`/`image2`) is **gone**:

```
sda1 ONL-BOOT (127M ext2)   sda2 ONL-CONFIG (128M ext4)
sda3 ONL-IMAGES (1G ext4)   sda4 ONL-DATA (28.2G ext4)
```

U-Boot `bootcmd` → `nos_bootcmd` (rewritten by the ONL installer to load
`arm-accton-as4610-54-r0.itb` from `usb 0:1`). The original ICOS `bootimage1=ext2load
usb $usbdev:2 … image1` env vars are still present but **stale** (sda2 is ONL-CONFIG now).
So booting ICOS requires restoring `sda.gpt` + `sda1` + `sda2` + `mtd2_env` from backup.
U-Boot (`mtd0`) + ONIE (`mtd3`) are untouched and not reflashed (a bad U-Boot bricks the
box). Disk GUID `6A9387CD-…`, MAC `04:F8:F8:15:A8:40`, ONIE `arm-accton_as4610_54-r0`.
