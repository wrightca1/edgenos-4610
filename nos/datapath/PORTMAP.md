# AS4610-54T — front-panel ↔ chip port map

> **⚠️ The copper silkscreen→ge map is NOT sequential, and is still being mapped
> empirically.** An earlier version of this file claimed "jack N = ge(N−1)"
> (from an unverified comment in `edged.c`). **Hardware testing disproved it.**
> None of our captured sources actually contain the copper silkscreen layout —
> ICOS `phy info` only lists the SDK *logical* `geN` numbers, ONLP only maps the
> SFP+ ports, and `config.bcm` only has MDIO addresses. The real map must be
> built by plugging a cable into each physical jack and reading which `geN`
> lights up (bcmd's link summary in `/tmp/bcmd.log` does exactly this).

## SFP+ / stacking (user-confirmed)
| Front panel | SDK name (netdev) | Logical port | PHY | MDIO / bus |
|---|---|---|---|---|
| SFP+ **49** | **xe0** | 50 | BCM84758 | 0x40 / 0xc1 |
| SFP+ **50** | **xe1** | 51 | BCM84758 | 0x41 / 0xc1 |
| SFP+ **51** | **xe2** | 52 | BCM84758 | 0x42 / 0xc1 |
| SFP+ **52** | **xe3** | 53 | BCM84758 | 0x43 / 0xc1 |
| Stacking **53–54** | **xe4–xe5** | 54–55 | Warpcore (internal) | — |

## Copper (48× BCM54282) — empirically verified so far
| Physical port | SDK name (netdev) | Notes |
|---|---|---|
| **1** | **ge25** | confirmed on hardware |
| **2** | **ge24** | confirmed on hardware |
| 3–48 | **TBD** | needs empirical mapping |

**Observed pattern (preliminary):** adjacent physical jacks are **pair-swapped** —
pair (1,2) → (ge25, ge24) — and ports 1–8 appear to live in the **ge24–31** MDIO
bank, i.e. the bank order is also scrambled vs. the silkscreen. Full permutation
pending more data points (ports 3, 9, 48 …). Logical port 49 (ge48) is internal.

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
