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

License note: Broadcom source-available (same class as OpenMDK/OpenBCM) — fine to
use/build on our own hardware; kept local, not committed (see LICENSING.md).
