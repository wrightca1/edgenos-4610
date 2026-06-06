# AS4610-54T — electrical ports & PHY verification vs ICOS reference

Cross-check of `edged`'s electrical-port (48× 1G copper RJ-45) and PHY handling
against the **authoritative full-SDK reference** read live from stock ICOS 3.4.3.7
(`bcmsh` `phy info` + `config show`, 2026-06-06; raw in
`../../live-investigation/dumps/icos_linked_2026_06_06/`). Conclusion first:

> **VERIFIED — edged's MDIO address map matches ICOS byte-for-byte for all 48 copper
> PHYs (BCM54282) and all 4 SFP+ PHYs (BCM84758), on the correct external MIIM buses.**
> edged empirically reaches all 48 copper PHYs (id `600d:845b`) + all 4 SFP+ PHYs
> (ucode loads, `fw-checksum 0x600d`); jack-to-jack copper links at 1G. The one
> improvement surfaced is SFP+ media config (`phy_fiber_pref`/`automedium`, below).

## 1. Authoritative PHY map (ICOS `phy info`)
54282 octal copper packages, the internal SGMII PHY, the 4× 84758 SFP+, and the
2× Warpcore QSFP. `addr` = MDIO phy address; bus = external MIIM bus.

| diag port | front jack | PHY | id0:id1 | MDIO addr | bus (ICOS iaddr) |
|---|---|---|---|---|---|
| ge0–7 | 1–8 | BCM54282 | 600d:845b | 0x01–0x08 | EBUS0 (0x81) |
| ge8–15 | 9–16 | BCM54282 | 600d:845b | 0x0b–0x12 | EBUS0 (0x89) |
| ge16–23 | 17–24 | BCM54282 | 600d:845b | 0x15–0x1c | EBUS0 (0x91) |
| ge24–31 | 25–32 | BCM54282 | 600d:845b | 0x21–0x28 | EBUS1 (0xa1) |
| ge32–39 | 33–40 | BCM54282 | 600d:845b | 0x2b–0x32 | EBUS1 (0xa9) |
| ge40–47 | 41–48 | BCM54282 | 600d:845b | 0x35–0x3c | EBUS1 (0xb1) |
| ge48 | (internal) | Phy56XXX | 0:0 | 0xe3 | internal |
| xe0–3 | SFP+ 49–52 | BCM84758 | 600d:86f0 | 0x40–0x43 | EBUS2 (0xc1) |
| xe4/xe5 | QSFP 53/54 | Warpcore WC-B1 | 143:bff0 | 0xc5/0xc9 | internal SerDes |

The `addr` column = `config.bcm` `port_phy_addr_geN/xeN` (verified: full sorted list
in `09_config_show.txt`). The **0x20 bit** in the copper addresses marks the upper
bank (jacks 25–48) — the board splits the 48 copper PHYs across **two external MIIM
buses**: jacks 1–24 → EBUS0, jacks 25–48 → EBUS1.

## 2. edged cross-check — MATCH
edged's `_phy_addr()` (`bcm956340k_miim_ext.c`, patch 03) uses the table
`_ge_phy_addr[48] = {0x01..0x08, 0x0b..0x12, 0x15..0x1c, 0x21..0x28, 0x2b..0x32,
0x35..0x3c}` — **identical** to ICOS `port_phy_addr_ge0..47`. It then drives:
- jacks 1–24 → `CDK_XGSM_MIIM_EBUS(0) | addr` (EBUS0, addr 0x01–0x1c)
- jacks 25–48 → `CDK_XGSM_MIIM_EBUS(1) | (addr & 0x1f)` (EBUS1, addr 0x01–0x1c; the
  0x20 bank-marker bit moved into the EBUS(1) select — the correct interpretation)
- SFP+ → `CDK_XGSM_MIIM_EBUS(2) | 0x00–0x03` (84758)

CDK encoding (`cdk/.../xgsm_miim.h`): `EBUS(b) = b<<6`, `INTERNAL = 0x80`. So edged
passes *(external bus, MDIO phy addr)* — a different composite byte than ICOS's display
`iaddr`, but the **physical MDIO address + bus are the same**. Empirically confirmed:
`edged --up-check` → all **48** copper PHYs answer `600d:845b` at the right jacks, and
the SFP+ 84758 ucode loads on all 4 (`fw-checksum 0x600d`). ✅
(Note: the 84758 sits at MDIO 0x00–0x03 on its bus; ICOS displays 0x40–0x43 with the
bus bit folded into the address — same device, different display convention.)

## 3. QSGMII octal-package structure (`phy_port_primary_and_offset_geN`)
The 54282 is an **octal** PHY: 6 packages of 8 ports. ICOS records each package's
**primary** MDIO address + per-port offset (format `0xPPOO`):

| package | jacks | primary addr | offsets |
|---|---|---|---|
| 1 | 1–8 | 0x01 | 0–7 |
| 2 | 9–16 | 0x09 | 0–7 |
| 3 | 17–24 | 0x11 | 0–7 |
| 4 | 25–32 | 0x19 | 0–7 |
| 5 | 33–40 | 0x21 | 0–7 |
| 6 | 41–48 | 0x29 | 0–7 |

OpenMDK's `bcm54282` driver discovers the package from the per-port address and inits
the octal core internally — edged binds it per-port after the post-`bmd_init` re-probe,
and copper links, so package init is functioning. (Primaries are recorded here in case
a future per-package init issue needs them.)

## 4. BCM54282 config registers — reference (ICOS, clause-22)
`phy ge0` (down) vs `phy ge25` (linked) — the **config** registers are identical; only
status/link-partner registers differ with link state:

| reg | meaning | value (both) |
|---|---|---|
| 0x00 | MII control | **0x1140** (autoneg-enable + 1000 + full-duplex) |
| 0x04 | AN advertisement | 0x0141 |
| 0x09 | 1000BASE-T control | **0x0600** (advertise 1000-T full + half) |
| 0x01 | MII status | 0x79ed linked / 0x79c9 down (link bit reflects state) |

edged's copper links at **1G full-duplex** → its 54282 (via OpenMDK `bmd_phy_init`)
advertises the same and autoneg completes. Config matches functionally. ✅
Relevant `config show` knobs: `phy_sgmii_autoneg_ge=1` (MAC↔PHY SGMII autoneg — edged's
copper links, so its SGMII is up), `port_phy_addr_geN` (matched).

## 5. SFP+ (BCM84758) — map verified; one real config improvement
- Addressing: xe0–3 = MDIO 0x40–0x43 (ICOS) ↔ edged EBUS2 0x00–0x03 — same PHYs,
  verified (ucode loads on all 4). ✅
- **Improvement (the 10G-link lead):** ICOS sets **`phy_fiber_pref_xe0..3=1`** +
  **`phy_automedium_xe0..3=0`** — forces the 84758 into **fiber media** (no copper
  auto-detect). edged should apply the OpenMDK equivalent before bring-up; this is the
  leading candidate for why edged's SFP+ media side doesn't lock (see
  `../../live-investigation/dumps/icos_linked_2026_06_06/FINDINGS.md` and
  `WARPCORE_10G_INVESTIGATION.md`). Also `bcm56340_4x10=1` is the SDK-clean 4×10G
  selector (vs our static `port_speed_max` table edit, patches 01/02).

## Bottom line
The **electrical (copper) ports and their PHYs are fully correct** in edged — addresses,
buses, octal-package handling, and 54282 autoneg config all match the ICOS reference and
work on hardware. The only PHY-side gap is the **SFP+ fiber-media config**, which is the
already-identified 10G-link lead, not a copper/electrical issue.
