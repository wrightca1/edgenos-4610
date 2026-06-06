# SFP+ module EEPROM + DOM + compatibility (2026-06-06)

Two 10G SFP+ modules plugged into the AS4610-54T uplink cages (xe0 = i2c-2,
xe1 = i2c-3), read live over the SFP i2c mux (A0h @0x50 base EEPROM, A2h @0x51
diagnostics). Pulled to confirm the modules are compatible before chasing the
10G link in the chip.

## Identity (A0h)

| Field | xe0 (i2c-2) | xe1 (i2c-3) |
|---|---|---|
| Vendor / PN | FINISAR `FTLX1370W3BTL-E8` | DELTA `LCP-10G3B4QDRME2` |
| Identifier (b0) | 0x03 SFP | 0x03 SFP |
| Connector (b2) | 0x07 LC (duplex) | 0x07 LC (duplex) |
| 10G compliance (b3) | 0x20 = **10GBASE-LR** | 0x20 = **10GBASE-LR** |
| Nominal bit rate (b12) | 0x67 = **10.3 Gbps** | 0x67 = **10.3 Gbps** |
| Wavelength (b60-61) | 0x051e = **1310 nm** | 0x051e = **1310 nm** |
| SMF reach (b14/15) | 1.4 km (LRL "lite") | 10 km |
| Encoding | 64B/66B | 64B/66B |

Both are **standard dual-fiber 1310 nm 10GBASE-LR** (symmetric wavelength +
duplex LC ⇒ *not* BiDi). Reach differs (1.4 km vs 10 km) but that's irrelevant
over a short patch. **→ Compatible with each other.**

Datasheets: Finisar FTLX1370W3BTL = 10GBASE-LRL 1310nm 1.4km duplex-LC; Delta
LCP-10G3B4QDR = 10GBASE-LR SFP+ (Delta product portal / Internet Archive manual).

## Live diagnostics (A2h DOM, internally calibrated)

| | xe0 Finisar | xe1 Delta |
|---|---|---|
| Temperature | 38.9 °C | 42.5 °C |
| Vcc | 3.41 V | 3.39 V |
| TX bias | 47.9 mA | 37.1 mA |
| **TX power** | −2.0 dBm (laser ON) | −2.0 dBm (laser ON) |
| **RX power** | −1.5 dBm (light present) | ~+3 dBm (light present) |
| RX_LOS | 0 | 0 |

Both modules transmit and receive optical power with no loss-of-signal — the
**optical layer is physically up**. (Note: the Delta RX of ~+3 dBm is higher than
the Finisar TX of −2 dBm, which is physically impossible if those two ports are
fibered *directly to each other* — so the fiber topology is worth confirming:
are xe0↔xe1 patched to each other, or is each going elsewhere/looped?)

## Conclusion

Modules compatible + optics good ⇒ the SFP+ 10G link failure is **not** the
modules or fiber; it is the **chip-side electrical path**. Per the open-source
robo2 `phy_84740_link_get`, 84758 link = (84758 PMD link) AND (internal Warpcore
SerDes link) — so the remaining work is the **Warpcore 10G lane lock** (+ the
84758 media/system PCS), the same per-lane RX-cal layer noted in the 40G QSFP
work. The 84758 optical-signal config itself is now correct in `edged`
(`sfp_tx_enable`) after pulling the real register map from the robo2-xsdk
`phy84740.h` (fixed `OPTICAL_SIG_LVL` = `0xc800`, levels b8/b9 — OpenMDK omits
these, which had pinned `sigdet=0`).

Raw hexdumps: see git history / `i2cdump -y -f {2,3} 0x50 b` and `0x51 b`.
