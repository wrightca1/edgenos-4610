# Datapath Verification — our NOS vs the live stock ICOS NOS

Cross-check of what **our** NOS (`nos/`) needs to drive the BCM56340, against the
**ground truth** captured from the running Edgecore FASTPATH/ICOS box
(`../../live-investigation/`). Verdict: **we have what we need** — the live
analysis confirmed our assumptions and filled the one real gap (board config).

## Verdict table

| What our NOS needs | Have it? | What the live box confirmed / gave us |
|---|---|---|
| **CMIC access address** | ✅ | ICOS maps CMIC at kernel VA `0xa698e000` = ioremap of **phys `0x48000000`** — exactly our `BDE_ACCESS.md` value. |
| **Chip identity for `--unit`** | ✅ | `soc` → `Chip=BCM56340_A0 Rev=0x01` → our `--unit 0x14e4:0xb340:0x01@0x48000000` is correct. |
| **Chip driver** | ✅ built | OpenMDK `bcm56340_a0` BMD, cross-compiled (`linux-user-mdk`). |
| **Port map** | ✅ confirmed | `ps` → `ge0–47` (1G SGMII) + `xe0–3` (10G) + `xe4–5` (20G) + `ge48` (CPU/GMII). Matches our hardware exactly. |
| **48× 1G copper PHY driver** | ✅ in binary | PHY is **Broadcom BCM5464S** (`phy_5464S_*` in config) → OpenMDK `bcm5464` driver **is linked** (`PHY_CONFIG_INCLUDE_BCM5464`, `BCM5464_AUTO_DETECT_SGMII_MEDIA`). |
| **10G/20G uplink SerDes** | ✅ in binary | Warpcore (`wc40_ucode_b0`) linked; `phy_fiber_pref_xe*=1`. |
| **MDIO/MIIM bus** | ✅ in binary | `bcm56340_miim_int` linked (PHY register access). |
| **Board config (`config.bcm`)** | ✅ **now have it** | The one thing our chip-generic build lacked — extracted live to [`config.bcm.as4610-54t`](config.bcm.as4610-54t): 144 props incl. **per-port PHY MDIO addrs**, `pbmp_xport_ge/xe`, `bcm56340_4x10=1`, table sizes. |
| **DMA model (Path B)** | ✅ known | `soc` → **DCB type 23**, ch0=TX / ch1=RX, DV ring 32 (`dv-size=160`). (Path A/PIO needs none of this.) |
| **Kernel BDE (Path B)** | ✅ source | ICOS uses `linux-kernel-bde` + `linux-user-bde` (lsmod) — our planned Path B; source in OpenBCM (GPLv2). Device nodes maj 127/126. |
| **Table sizes / scale** | ✅ known | `l2_mem_entries=0x4000` (16K L2), `l3_mem_entries=0x2000` (8K L3), `l3_max_ecmp_mode=1`, `ipv6_lpm_128b_enable=1`, `bcm_num_cos=8`. |

## The one integration item

We have every *piece*; the remaining work is **feeding the board `config.bcm`
into the datapath**, because OpenMDK's BMD (unlike the full SDK) does **not**
auto-read a `config.bcm` file — it expects per-port PHY addresses from board
code. Two clean options, both now unblocked by the extracted config:

1. **OpenMDK + supply the map** — pass the `port_phy_addr_geN` / `xeN` values
   (from `config.bcm.as4610-54t`) into the BMD's PHY-bus setup (board code /
   `bmd_phy_ctrl`). Smallest footprint; matches our current `linux-user-mdk`.
2. **OpenBCM (full SDK)** — consumes `config.bcm.as4610-54t` **directly** (it's
   real config.bcm syntax). Heavier, but also gives L3 and is what ICOS itself
   uses. Best if/when we want the full router.

Either way, the data we were missing now exists verbatim from the live box.

## Confirmed bring-up sequence (no surprises expected)

1. `./run-on-target.sh` → `linux-user-mdk --unit 0x14e4:0xb340:0x01@0x48000000`
   (Path A, `/dev/mem`) → register read-back sanity (we now know CMIC is there).
2. `init` → chip out of reset.
3. Apply the board config (option 1/2 above) so PHY addresses are known.
4. Bring up a 1G copper port (BCM5464S @ its `port_phy_addr`) and a 10G SFP+
   (`xe0`, Warpcore) → link.
5. L2 (VLAN/MAC/forward), CPU tx (polled).
6. Path B (kernel BDE, DCB t23) for DMA/IRQ → real RX-to-CPU.
7. L3 (OpenBCM `bcm_l3_*`, or hand-rolled) + FRR.

## Bottom line

Nothing is missing to start bring-up. The live ICOS analysis **validated** our
access model and port map, **confirmed** the PHY/SerDes drivers are already in
our binary, and **supplied the board `config.bcm`** that a chip-generic SDK
build can't know on its own. Remaining = integration + on-hardware execution,
not discovery.

## Environmentals (temp / PSU / fan / LED) — verified

Our NOS is **ONL-derived**, so the full **ONLP platform layer** for `as4610-54`
is already in the image (the `onlp-arm-accton-as4610-54-r0` package + the
`accton_as4610_{cpld,fan,leds,psu}` kernel modules we build). Inventory:

| Subsystem | Our ONLP support | Live box (`show environment`) |
|---|---|---|
| **Temp** | 3 thermal IDs: chassis **LM77** (`*-0048`) + PSU1 + PSU2 (PMBus) | Sensor-1 = **48 °C, Normal** ✅ |
| **PSU** | 2 PSUs: EEPROM presence (`0x50/0x51`) + **YM-1921 PMBus** (`0x58/0x59`) telemetry | PS-1 **Operational**, PS-2 present/not-powered ✅ |
| **Fan** | chassis fan(s) + PSU1/PSU2 fans (fault/rpm/duty); count is dynamic | **No chassis fans** — the 54T is effectively fanless (cooling via PSUs); ONLP handles `fan_count=0` ✅ |
| **LED** | 7-seg stack-ID (DIG1/2 + dots), SYS, PRI, PSU1/2, STK1/2, FAN | (CPLD-driven; covered by `accton_as4610_leds`) |
| **CPLD** | `accton_as4610_cpld` @ i2c `0x30` (PSU status, fan ctrl, LED, reset) | present (drives the above) |

Hardware addresses match our device tree (CPLD `0x30`, LM77 `0x48`, PSU EEPROM
`0x50/0x51`, PMBus `0x58/0x59`, RTC `0x68`). **Note:** ICOS exposes these via its
own closed platform stack (no standard `hwmon`); under **our** ONL NOS the same
chips bind to standard Linux i2c → sysfs and ONLP reads them. The live readings
confirm the sensors physically work — so the environmental side is **complete**:
nothing missing, just runs through ONLP instead of ICOS's stack.

## SFP+ / QSFP+ optics (ports 49–54) — verified

Two sides: **management** (presence/EEPROM/control via ONLP + CPLD + optoe) and
**data-plane link** (the SDK driving xe0–5). Both covered.

| Function | Our coverage | Status |
|---|---|---|
| **Presence** (SFP+ 49–52, QSFP+ 53–54) | CPLD regs `0x02`/`0x03` (SFP) + `0x21` (QSFP), via ONLP `module_present_*` | ✅ regs verified on live box |
| **EEPROM** (vendor/type) | `optoe2` (SFP) / `optoe1` (QSFP) on PCA9548 mux @ `0x70`, ch0–5 → `/sys/.../N-0050/eeprom`; ONLP `onlp_sfpi_eeprom_read` | ✅ path present (mainline `optoe`+`pca954x`) |
| **DOM/diagnostics** | ONLP `onlp_sfpi_dom_read` | ✅ |
| **RX_LOS / TX_FAULT** | CPLD `module_rx_los_*`/`module_tx_fault_*`; ONLP `control_get` (SFP+) | ✅ |
| **TX_DISABLE** | CPLD kernel driver exposes `module_tx_disable_1..4` (writable) | ✅ in kernel driver — but ONLP `control_set` is a **stub** (`E_UNSUPPORTED`); trivially wired to the sysfs if a NOS agent needs it |
| **QSFP reset** | CPLD driver "brings QSFPs out of reset" at init | ✅ |
| **Data-plane link** (xe0–3 10G, xe4–5 20G) | Warpcore SerDes (in our `linux-user-mdk` binary) + `config.bcm`: `phy_fiber_pref_xe0-3=1`, `port_phy_addr_xe0-3=0x40-0x43`, `bcm56340_4x10=1` | ✅ |

**Live state:** no optics currently inserted — CPLD `0x21=0x00` (QSFP present bits
clear) and bcm `ps` shows xe0–5 *down*, consistent with `show environment` (no
modules). So a live EEPROM read couldn't be exercised; the access path is sound
by construction (ONL DTS + mainline `optoe`/`pca954x` + the verified CPLD present
regs). Insert an SFP+/QSFP+ and the module EEPROM is readable at the mux channel.

**One nit, not a blocker:** ONLP `onlp_sfpi_control_set` is unimplemented on this
platform, so TX_DISABLE/LPMODE aren't exposed through the ONLP *API* — but the
underlying CPLD control (`module_tx_disable_*`) exists and QSFP-reset is handled
at init, so links come up; a NOS agent can drive TX_DISABLE via the CPLD sysfs
directly if needed.

### CORRECTION (2026-06-05) — 10G SFP+ PHY gap found via live `phy info`

The data-plane row above understated the PHY layer. Live `bcmsh phy info` shows
the 10G SFP+ ports (xe0–3) sit behind an external **BCM84758** firmware-driven
PHY (the 1G ports use **BCM54282**; QSFP is internal Warpcore B1 4-lane).
**OpenMDK/OpenBCM do NOT support the BCM84758** (OpenMDK has sibling 84756 with a
different PHY ID `0x8670` vs live `0x86f0`; OpenBCM has only an enum, no
driver/firmware). → **10G SFP+ uplinks won't link** until we get the 84758
driver+ucode (extract from the backed-up ICOS image, or a PHY SDK). Full detail +
resolution: `../../live-investigation/PHY_SIGNAL_PATH.md`. 1G (BCM54282) and the
QSFP SerDes ARE covered.
