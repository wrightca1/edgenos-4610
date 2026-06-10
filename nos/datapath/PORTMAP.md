# AS4610-54T — front-panel ↔ chip port map (verified)

Authoritative mapping between **front-panel silkscreen numbers** and the Broadcom
**BCM56340** SDK port names / logical port IDs. Cross-verified across three
sources that all agree: the live **ICOS 3.4.3.7** `phy info` capture
(`live-investigation/dumps/icos_linked_2026_06_06/19_phy_map_and_copper.txt`),
`config.bcm.as4610-54t` (`port_phy_addr_*`), and `mdk-app/edged.c` (`ge_phy_addr[]`,
"front-panel jack = geN+1").

`bcmd` brings up **every** port as a Linux netdev named by the SDK name below.

## Mapping

| Front panel (silkscreen) | SDK name (netdev) | Logical port | PHY | MDIO addr / bus |
|---|---|---|---|---|
| Copper jack **N** (1–48) | **ge(N−1)** | **N** | BCM54282 | see below |
| SFP+ **49** | **xe0** | 50 | BCM84758 | 0x40 / 0xc1 |
| SFP+ **50** | **xe1** | 51 | BCM84758 | 0x41 / 0xc1 |
| SFP+ **51** | **xe2** | 52 | BCM84758 | 0x42 / 0xc1 |
| SFP+ **52** | **xe3** | 53 | BCM84758 | 0x43 / 0xc1 |
| Stacking **53** | **xe4** | 54 | Warpcore (internal) | — |
| Stacking **54** | **xe5** | 55 | Warpcore (internal) | — |

**Copper is sequential**: jack 1 = ge0, jack 26 = ge25, jack 48 = ge47.
Logical port 49 (ge48) is internal — not a front jack.

### Copper MDIO addresses (octal BCM54282, gapped — NOT a formula)
| SDK | jacks | MDIO bus | MDIO addrs |
|---|---|---|---|
| ge0–ge7   | 1–8   | 0x81 | 0x01–0x08 |
| ge8–ge15  | 9–16  | 0x89 | 0x0b–0x12 |
| ge16–ge23 | 17–24 | 0x91 | 0x15–0x1c |
| ge24–ge31 | 25–32 | 0xa1 | 0x21–0x28 |
| ge32–ge39 | 33–40 | 0xa9 | 0x2b–0x32 |
| ge40–ge47 | 41–48 | 0xb1 | 0x35–0x3c |

## Notes
- ⚠️ The port previously called "port 1" in testing actually came up as **ge25**,
  which on the silkscreen is **jack 26** (cabling/label, not silkscreen jack 1).
- All ports default to **VLAN 1** in `bcmd`, so the chip L2-bridges any ports
  sharing an external segment — keep different networks on different ports, and
  never two ports on the mgmt 10.1.1 LAN (L2 loop). Per-port VLAN/L3 isolation
  comes with the planned netlink→chip sync layer.
- Configure in Linux, e.g.: `ip addr add 10.14.1.2/24 dev ge25` then `ping …`.
