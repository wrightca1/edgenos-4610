# AS4610-54T physical signal path — the REAL optics bring-up story

Bringing optics up is **not** just EEPROM/presence. The hard part is the analog
signal path: external PHYs/retimers, firmware, per-lane SerDes tuning, link
training, FEC. This doc captures the **actual** PHY chain on the AS4610-54T, read
live from the running ICOS box (`bcmsh phy info`), and corrects our earlier
(datasheet-based, wrong) "no retimers / simple SerDes" claim.

## The actual PHY chain (live `phy info`, [`dumps/bcm_phy_info.txt`](dumps/bcm_phy_info.txt))

| Port(s) | Speed | PHY in the path | MDIO | Notes |
|---|---|---|---|---|
| `ge0–47` | 1G copper | **BCM54282** ×6 (octal copper) | 0x01–0x3c | external PHYs |
| `ge48` | — | internal (`Phy56XXX`) | — | CPU port |
| **`xe0–3`** | **10G SFP+** | **BCM84758** (Quad SFI-XFI **firmware** PHY) | 0x40–0x43 | **gearbox/retimer-class — needs ucode** |
| **`xe4–5`** | **20G QSFP+** | **Warpcore B1, 4-lane** (`WC-B1/14/4`,`/15/4`) | 0xc5/0xc9 | internal SerDes, multi-lane |

## What we have vs what we DON'T

| PHY | Role | OpenMDK | OpenBCM 6.5.27 | In our binary? | Verdict |
|---|---|---|---|---|---|
| **BCM54282** | 48× 1G | ✅ driver+ucode | — | ✅ `PHY_CONFIG_INCLUDE_BCM54282` | **covered** |
| **Warpcore B1** | QSFP 20G SerDes | ✅ `wc40_ucode` | ✅ | ✅ linked | SerDes covered; **multi-lane tuning = real work** |
| **BCM84758** | 10G SFP+ | ❌ only sibling **84756** (covers 756/757/759, PHY ID1 `0x8670`) | ❌ **enum only** (`_phy_id_BCM84758` in phy.h) — **no driver, no firmware** | linked 84756 (wrong) | ❌ **GAP** |

**The gap is concrete:** the live 84758 reports PHY ID1 = **`0x86f0`**; OpenMDK's
84756 driver matches **`0x8670`** → it will not bind/drive the 84758, and there is
**no 84758 firmware** in either OpenMDK or OpenBCM. So with our current open
stack, **the 4× 10G SFP+ uplinks will not come up.** This is the 4610's analogue
of the 5610's BCM84740-firmware / CL82 wall — exactly the class of problem that
isn't visible from reading EEPROMs.

## Why our earlier docs were wrong

`BOARD_TOPOLOGY_AND_I2C.md` / `ASIC_AND_CPU_ARCHITECTURE.md` / `NOS_BUILD_PLAN.md`
said "no retimers, integrated/simple SerDes" — that was inferred from the
datasheet + the ONL DTS (which only models the optics **EEPROMs**, not the
BCM84758 PHY, because ONL drives the PHY via the SDK, not the device tree). The
live `phy info` shows the truth: there **is** a firmware-driven 10G PHY in the
SFP+ path. Lesson: the DTS/EEPROM view hides the analog path.

## Resolution paths for the BCM84758 (10G SFP+)

1. **Extract the 84758 firmware + init from the live ICOS** (best — we own the
   reference): ICOS drives the 84758, so its driver + ucode live in `switchdrvr`
   inside the **backed-up image** (`backup/sda2.img.gz` → `image1`). Mine the
   ucode blob + the init sequence (same method as the 5610's 84740 firmware).
2. **Source BCM84758 from a Broadcom PHY SDK** (the 8475x "Quad SFI-XFI" family;
   may appear under a newer phymod/epdm release or a codename).
3. **Adapt the OpenMDK 84756 driver to 84758** (same family) — but still needs the
   84758 **ucode** (firmware differs per device), so (1) is the real unblock.

## QSFP (xe4–5) multi-lane — also real, but supported

Warpcore B1 4-lane is in our binary; bringing the 20G QSFP up still needs the
lane map + per-lane SerDes tuning (TX FIR/pre-emphasis) + any FEC — captured in
the live `config.bcm` (`../../nos/datapath/config.bcm.as4610-54t`) and exercised
by the Warpcore ucode. Lower risk than the 84758, but not "free."

### Lane wiring DECODED (2026-06-05) — all 4 lanes wired per cage, bonded as one 20G port

Resolved how the QSFP cages are laned, from three corroborating sources:

1. **CDK chip block table** (`OpenMDK/.../bcm56340_a0_chip.c`): the BCM56340 has
   Warpcore (XWPORT) blocks, each = **4 physical ports = 4 SerDes lanes**:
   - block 13 → phys 53–56  → the **four 10G SFP+** (xe0–3): one Warpcore *split*
     into 4 independent single-lane 10G ports.
   - block 14 → phys 57–60  → **QSFP1 (xe4)**, a whole dedicated Warpcore.
   - block 15 → phys 61–64  → **QSFP2 (xe5)**, a whole dedicated Warpcore.
2. **Name format** decoded via the 5610 Rosetta (`edgecore-5610-reverse-
   engineering/.../10_phy_info_xe0.txt` + `SERDES_WC_INIT.md`): legacy format is
   `WC-<rev>/<block>/<lane>`. On the 5610, a 4-lane Warpcore appears as **four**
   lines `WC-B0/16/0..3` (4 single-lane 10G ports). On the **4610**, block 14
   appears as **one** line `WC-B1/14/4` (and block 15 as `WC-B1/15/4`) — a single
   bonded port, the trailing `4` = **lane width 4**, not a lane index (indices are
   0–3).
3. **MDIO spacing**: xe0–3 are consecutive (0x40–0x43, 1 addr each); xe4=0xc5 and
   xe5=0xc9 are spaced by 4 = one full 4-lane block each.

**Conclusion:** each QSFP cage has its **own dedicated 4-lane Warpcore, all 4
lanes wired and bonded** into a single logical port. 20G = **4 lanes × ~5 Gbps**
(not 2×10G). The 20G ceiling is the chip's **128 Gbps fabric budget**
(48×1G + 4×10G + 2×20G = 128, exactly non-blocking), NOT a lane shortage.

**Implication — NO standard 40G (corrected; supersedes an earlier wrong claim).**
The SerDes lanes are electrically 40G-class (the chip's 42G configs prove these
4 lanes can clock ~10G each), but the 4610 cannot present a standard 40G port:

1. **Port-config / fabric cap.** The BCM56340 BMD has 14 SKU port-config maps
   (`bcm56340_a0_bmd_attach.c`, `port_speed_max_*`). The AS4610-54T uses the
   **4×10G + 2×HiGig-21G** layout — live box reports the QSFPs at 20G (code 21).
   That fills the chip exactly: 48×1G + 4×10G + 2×20G = **128 G = full fabric**.
   The only 40G-class configs (450/490/491/495/500) are **pure-uplink layouts
   (~3×42G ports, no 48×1G+4×10G)** — they don't fit this board, and 2×42G on top
   of the 4610's port set = 172 G ≫ 128 G fabric. So 40G is not a selectable mode
   on this board.
2. **It's HiGig, not Ethernet.** Even the chip's top speeds are coded **21/42 =
   HiGig2 20G/40G** (Broadcom proprietary stacking framing) — **never `40000`
   (IEEE 40GBASE-R)**. HiGig has a proprietary header and does **not**
   interoperate with a standard 40G device. (Stock ICOS actually runs these as
   20G **Ethernet/XGMII** with `pbmp_gport_stack=0`, but 20G is itself a
   non-standard rate — no standard device speaks 20G either.)

**Bottom line:** these cages are hard-capped at **20G** by the board's chip
config and are stacking-class — they will **not** run 40G and will **not**
interoperate with a standard 40G device. (A QSFP could be configured as a single
**10G** lane (standard 10GBASE-R) via a QSFP→SFP+ breakout to talk to a 10G
device — that's the only standard-Ethernet interop these cages offer.)

> Cabling consequence: a QSFP+ **DAC** (all 4 pairs) is fully used at 20G (4×5G)
> for stacking. A 40G **SR4/LR4 optic** won't work — not for lack of lanes (all 4
> are wired) but because the port is 20G/HiGig-class, not 40GBASE-R.

### USEFUL reconfiguration — turn each QSFP cage into standard 10G port(s)

What the cages *can* do for us (in our own NOS): be re-flexported into standard
**single-lane 10GBASE-R / SFI** ports.

- **Proof it works:** the 4 native SFP+ ports (xe0–3) are already exactly this —
  Warpcore block 13 split into 4 independent 1-lane 10G ports. Blocks 14/15 (the
  QSFP cages) are identical 4-lane Warpcores → same split applies. BMD allows it
  (`speed_max`=21G ≥ 10000, `bmdPortMode10000fd`).
- **Fits the fabric:** 48×1G + 4×10G + 2×10G = **108 G** (< 128 G; frees 20 G vs
  the stock 2×20G).
- **Interoperable:** real 10GBASE-R, links with any standard 10G device (unlike
  the 20G/HiGig stock mode).
- **★ Sidesteps the BCM84758.** The QSFP cages wire **straight to the internal
  Warpcore — no external PHY** (xe4/xe5 = `WC-B1`; only xe0–3 go through the
  proprietary-firmware 84758). So 10G off the QSFP cages = direct Warpcore SFI,
  ucode already in our binary, **no 84758 driver/firmware dependency.**
- **More than 2 possible** via QSFP→SFP+ breakout (4 lanes/cage): line-rate
  sweet spot is 2 ports/cage (48+40+40 = 128 G exactly); all 8 (168 G)
  oversubscribes.
- **Effort:** a flexport / port-config (p2l/p2m/speed_max + TDM) change, not a
  live speed toggle. Templated by the 14 stock config maps (e.g. cfg 497 = 12×10G)
  in `OpenMDK/.../bcm56340_a0_bmd_attach.c`. Real work, but no architectural
  blocker.

## Status

- 1G copper: ✅ covered. QSFP SerDes: ✅ present (tuning TBD).
- **10G SFP+ (BCM84758): ❌ GAP — driver+firmware not in OpenMDK/OpenBCM.** Next
  step: extract from the backed-up ICOS image (we have it), or a PHY SDK.
- Full link bring-up (any port) still needs a module + link partner to validate
  training/FEC — can't be proven by config alone.

## Source search results (2026-06-05) — where the BCM84758 code is / isn't

Searched **every** local tree (OpenMDK, OpenBCM, ONL, open-nos-ref, newnos,
custom-nos, edgecore-5610-reverse-engineering):

- **No `84758`-named driver or ucode anywhere** as source.
- **OpenMDK has the full sibling family as SOURCE**: `bcm84756_drv.c` —
  *"PHY driver for BCM84756, BCM84757 and BCM84759"* ("Quad SFI-XFI PHY") — plus a
  **shared ~400 KB family ucode** (`bcm84756_ucode` + `_b0_ucode`). It matches
  PMA-PMD ID1 `0x8670` and reads a chip-ID reg (`0xc802/3`) → `CHIP_IS_BCM84756_FAMILY`.
- **OpenBCM 6.5.27**: only the enum `_phy_id_BCM84758` in `soc/phy.h` — legacy
  8475x drivers were dropped; no driver, no ucode.
- **84758 IS compiled into two binaries we hold**: the Cumulus `switchd` (5610
  repo — its `phy list` includes `BCM84758`) and the **live ICOS image**
  (`backup/sda2.img.gz` → `image1/switchdrvr`). Firmware recoverable from either.

### Reframed resolution — likely a driver-ID patch, not a firmware hunt

The 84758 is the **same family** as the well-supported 84756, and Broadcom's
8475x family ucode is typically shared. So the most promising path is:
1. **Adapt OpenMDK's `bcm84756` driver**: add the 84758 PMA-PMD ID1 (`0x86f0`) +
   chip-ID (`0x00084758`) to the match tables (`CHIP_IS_BCM84756_FAMILY`, devlist),
   and reuse the existing `bcm84756_ucode`. Try it on hardware.
2. **Only if the family ucode is rejected** by the 84758, extract the
   84758-specific ucode from the Cumulus `switchd` or the ICOS `switchdrvr`
   (both on disk) and pair it with the adapted driver.

This is far more contained than the 5610's PHY battle: we have the family driver
+ shared ucode as open source, and two binary fallbacks for the exact firmware.
**Still needs a 10G module + on-hardware test to confirm link/training.**

## Sourcing the BCM84758 — internet search + ICOS extraction (2026-06-05)

**Internet: not publicly available.** Broadcom's upstream OpenMDK on GitHub has
`bcm84756` but **no `bcm84758`**. OpenBCM 6.5.27 = enum only. The 84758 is a real
Broadcom Quad 10GbE SFI-XFI PHY (no MACsec; 84756 has it), gated behind Broadcom
docSAFE/contract — no open driver+firmware.

**ICOS extraction: the binary is now in hand.** Pulled the live ICOS `switchdrvr`
(58 MB, `backup/icos-extract/switchdrvr`, git-ignored) + `devshell_symbols.gz`
(646 KB). `switchdrvr` drives the 84758, so its driver+ucode are inside it. But:
- It is **stripped** (no symbol table); "BCM84758" is built from the chip-ID at
  runtime, not a static string — can't grep the ucode by name.
- The 84758 ucode is **NOT** OpenMDK's 84756 ucode — the 32 KB
  `bcm84756_ucode`/`_b0` blobs are **absent** from `switchdrvr` (verified). With
  `config.bcm` `phy_ext_rom_boot=0` (SDK downloads the ucode, not a PHY ROM), the
  84758-specific firmware lives in this binary.

**Carve = focused Ghidra RE next:** disassemble the 84758 PHY-init path in
`switchdrvr`, find the ucode data pointer + length, carve the blob — the 5610-class
work, but now we possess the binary that contains it (+ symbol file to assist).

## ✅ RESOLVED via Plan B (2026-06-05) — BCM84758 source found public, no carve needed

GitHub code search found the BCM84758 driver+firmware as **source** in public
full Broadcom XGS-SDK mirrors (incl. **Broadcom's own `robo2-xsdk`** + multiple
`sdk-xgs-robo-6.5.7` copies). Pulled the complete set to
`nos/datapath/phy84758-src/` (git-ignored; Broadcom source-available license):

| File | Role |
|---|---|
| `phy84740.c` (325 KB) | **the driver** — `#define PHY84740_ID_84758 0x84758`; loads `phy84758_ucode_bin` (the 84758 is in the **BCM84740 driver family**) |
| `phy84758_ucode.c` (205 KB) | **the firmware** — *"Firmware used by BCM84758 device's micro-Controller. Version 0128"* |
| `phyident.c/.h` | maps `_phy_id_BCM84758` (OUI `BRCM_OUI5`, model `0x2F`) → `phy_84740drv_xe` |
| `phy84756.*`, `phy84740_ucode.c`, fcmap/i2c | family support files |

**Key insight:** the 84758 is **84740-family** — the *same PHY family as the
5610* (BCM84740) — so our 5610 PHY RE transfers directly, and the SDK source
confirms the structure. So the carve (Plan A) is **not needed**; the
binary-extract route stays only as a last-resort cross-check.

### Integration path
The driver is the full-SDK `src/soc/phy/` style → cleanest with the **OpenBCM**
datapath (which we already want for L3): add `phy84740.c` + `phy84758_ucode.c` +
the `phyident` entry into OpenBCM `src/soc/phy/` (our 6.5.27 has the enum but not
these files; they come from 6.5.7 — minor port). With OpenMDK (different phy
framework) it'd need more adaptation, so this nudges the datapath toward OpenBCM.
Still requires a 10G module + on-hardware test to confirm link/training.

License note — TWO REVISIONS, read both:

- **First take (from the mirror):** the sysdevguru mirror's files read `(c) 2016
  Broadcom. Broadcom Proprietary and Confidential. All rights reserved.` with **no
  LICENSE file** → looked proprietary / non-redistributable.
- **CORRECTED 2026-06-05 (the decisive finding):** the **same files are published
  by Broadcom's OWN official GitHub org** —
  `Broadcom/Broadcom-Compute-Connectivity-Software-robo2-xsdk` — under a
  `Legal/LICENSE` that **grants** "worldwide, non-exclusive, royalty-free,
  perpetual… use, reproduce… distribute… and create derivative works and
  distribute such source code" — **word-for-word the OpenMDK/OpenBCM grant.** The
  official file header even points at that LICENSE. So this code **IS
  source-available and redistributable**, subject to the same conditions as
  OpenBCM (preserve notices, GPL-incompatible → keep kernel split, export
  control). We re-sourced from the official repo to
  `nos/datapath/phy84758-src/broadcom-official/` (with `LICENSE` + `PROVENANCE.md`).
  The "carve from our own ICOS" route is now only an internal cross-check, not the
  basis for use/distribution.

## ✅✅ VERIFIED 2026-06-05 — carved from our own ICOS, byte-for-byte identical to the mirror

Settled the license question by extracting the firmware from **our own** ICOS
`switchdrvr` (the copy that shipped on the box, under Edgecore's license to us as
the hardware owner) and comparing it to the mirror's `phy84758_ucode.c`:

- Parsed the C array → 32768-byte raw blob (`phy84758_ucode_bin_len /* 32768 */`).
- Found it **contiguously, in the clear** in `switchdrvr` at offset **`0x34de1bc`**
  (pure content match — no disassembly needed; the trailer is `00 08 41 64 01 28
  7d 7c` = 84740-family chip-ID + version 0128 + cksum, which is why the earlier
  84756-trailer `00 08 47 5x` search missed it).
- **SHA256 identical, 0 byte diffs:**
  `64ae5619b3625982141250299c573e734663133547c18f0bf29317ea0112f9cf`

Implications:
1. The public mirror is **genuine** — exact same firmware Edgecore ships (v0128).
2. We hold a **legitimately-licensed copy from our own device**; for running on
   this box we don't depend on the third-party mirror at all.
3. The "carve" (Plan A) is **trivial**, not the Ghidra RE feared in
   `SWITCHDRVR_ANALYSIS.md` — the blob is stored plainly in `.rodata`.

Blobs saved (git-ignored): `backup/icos-extract/phy84758_ucode_carved.bin`
(from ICOS) and `…_from_mirror.bin` (from the C source). For a publishable NOS,
ship the open driver and have the user supply the blob from their own device at
install (the `request_firmware()` separation), since the blob itself is not
redistributable.
