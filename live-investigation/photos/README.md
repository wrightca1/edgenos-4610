# AS4610-54T internal photos (2026-06-06)

Lid-off photos of the unit (S/N EC2025000934). Most silicon is under the two large
heatsinks (BCM56340 SoC + QSGMII PHY banks), so these mainly confirm board identity
and the visible support chips.

| File | What it shows |
|---|---|
| `board_top_overview.jpeg` | Whole mainboard, lid off — two heatsinks (SoC + PHY banks), power section, 48× RJ-45 + 6× SFP cages. |
| `board_front_panel.jpeg` | Front panel: 48 copper jacks + 4× 10G SFP+ + 2× 20G QSFP cages. |
| `cpld_lattice_machxo2_and_label.jpeg` | Lattice CPLD + board-label barcode: `ES4654BH-0917-EC (4610-54T-O-AC-Fv1) MAIN`, S/N `EC2025000934`. |
| `cpld_and_pcba_label.jpeg` | Clearer CPLD marking: **Lattice MachXO2 `LCMXO2-1200HC`**; PCBA `142000001517A REV:01`. |
| `mezzanine_fa202geu.jpeg` | Small daughtercard `FA202GEU-AC` (CPN `1071000002544A`) — likely PoE/clock mezzanine. |

Confirmed: CPLD = Lattice MachXO2 LCMXO2-1200HC (drives `accton_as4610_cpld` @ i2c0 0x30).
The BCM56340 SoC + Warpcore SerDes are under heatsinks (not visible).
