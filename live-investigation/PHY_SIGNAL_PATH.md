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
