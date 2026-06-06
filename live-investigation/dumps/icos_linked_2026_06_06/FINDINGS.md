# ICOS dynamic analysis with a LIVE 10G link (2026-06-06)

Reflashed the AS4610 back to stock ICOS 3.4.3.7 (see `../../RESTORE.md`) specifically
to analyze the SFP+ 10G datapath **with a live link** — the prior live-investigation
had no SFPs/no link. Two compatible 1310nm LR modules on xe0/xe1 (fiber xe0↔xe1).
Goal: find what ICOS's full Broadcom SDK does to lock the Warpcore 10G RX that our
open-source `edged` (OpenMDK) does not. Raw captures: this directory.

## THE headline result — the blocker is software, definitively

**ICOS brings BOTH fibered SFP+ ports UP at 10G on the exact same hardware** where
edged cannot:

```
CLI:   0/49  10G Full  10G Full  Up      0/50  10G Full  10G Full  Up
diag:  xe0   up  10G FD  Forward          xe1   up  10G FD  Block
```

Same modules, fiber, board, BCM56340, and Warpcore SerDes that edged leaves at
`RX_LINKSTATUS=0`. ⇒ The 10G blocker is **100% software** (OpenMDK's Warpcore RX
bring-up), **not** hardware, optics, fiber, or the modules. This retires every
hardware hypothesis.

## PHY map (diag `phy info`)
- `ge0..ge47` (ports 1-48): **BCM54282** copper, MDIO 0x01-0x3c on buses 0x81/89/91/a1/a9/b1.
- `ge48` (port 49): internal `Phy56XXX`.
- **`xe0..xe3` (ports 50-53): BCM84758** (id `600d:86f0`), MDIO 0x40-0x43, bus 0xc1.
- **`xe4/xe5` (ports 54-55): `WC-B1` Warpcore** (id `143:bff0`), the 2× QSFP stacking.
- For xe0-3 the Warpcore is the *internal* MAC-side SerDes (between MAC and the 84758).

## Actionable leads — SDK config ICOS applies that edged likely omits (`config show`)
These are the most directly useful findings (full dump: `09_config_show.txt`):

| Property | Value | Why it matters for edged |
|---|---|---|
| **`phy_fiber_pref_xe0..3`** | **`1`** | Forces the 84758 to **prefer/treat the port as fiber**. If edged leaves it in copper/auto media, the 84758 media side won't bring up on fiber. **Top suspect.** |
| **`phy_automedium_xe0..3`** | **`0`** | **Disables media auto-detect** — pin the port to the fiber medium. Pairs with fiber_pref. |
| `phy_5464S_xe0..3.0` | `0x1` | Per-port PHY property (media/SFP related) — investigate the OpenMDK equivalent. |
| **`bcm56340_4x10`** | **`1`** | The SDK's clean way to make the 56340 run **4×10G SFP+** — vs our static `port_speed_max` table hack (patches 01/02). Worth adopting. |
| `phy_ext_rom_boot` | `0` | PHY 8051 firmware comes from the **SDK image**, not an external ROM — matches our request_firmware/ucode approach. |
| `port_phy_addr_xe0..3` | `0x40..0x43` | Confirms our external-PHY MDIO map (patch 03). |
| `pbmp_xport_xe` | `0xfc000000000000` | xe (10G/20G) port bitmap. |

**Next edged experiment:** set the 84758 to **fiber media / disable automedium** before
link bring-up (the OpenMDK analogue of `phy_fiber_pref=1` + `phy_automedium=0`), then
re-check Warpcore `RX_LINKSTATUS`. This is the most promising lead from the locked box.

## DSC (Warpcore RX equalizer) — locked state, and a key negative
`phy diag xeN dsc` (`04_dsc_locked.txt`) for the **linked** xe0/xe1:
```
PLL Range 234 | per lane: PF=1 SL_TRGT=0x150 VGA=0x36 BIAS=100% DFE1-5=0
               MAIN=0x55 POSTC=0x08  (PE/ZE/ME/PO/ZO/MO vary per lane)
```
**Key negative:** the **down** port xe2 (no fiber) shows the **same** DSC values. So the
lock is **not** distinguished by these static DSC registers — DFE taps are 0 (clean
optical, no DFE needed) and VGA/MAIN/POSTC are identical linked-vs-down. ⇒ The
difference is in the **bring-up sequence / PCS-lock / uC firmware**, not a static DSC
value. (Reinforces that a register-by-register copy of DSC won't crack it.)

## 84758 register state when locked (`05_miim_syntax_test.txt`, `phy xe0`)
Standard MMDs captured: PMA/PMD(1) `0.0=0x2040`(10G) `0.7=0x0008`(10G-LRM) `0.8=0xb3e1`;
PCS(3) `0.0=0x2040` `0.8=0x8c01`; AN(7) `0x10=1 0x11=0xa0 0x12=0x4000`. (Vendor regs
>0x1f — `c8e4/c800/cd17` etc. — not in this dump; read separately if needed.) Compare
to edged's unlocked `../edged_upcheck_unlocked_baseline_2026_06_06.txt`.

## What did NOT work (so we don't repeat it)
- **Live BSL register trace is blocked on this build.** `debug soc miim|phymod|phy verbose`
  sets the levels, but the SDK's BSL output is **not routed to the console/diag tty**
  (confirmed: even `phy ge0`/`phy info` reads and a `port xe0 en=0/1` flap produced zero
  console trace; `show logging buffered` empty). So we **cannot** capture the live
  register-write *sequence* of the lock via the diag shell here. Would need to redirect
  BSL to console/file (sink config) or trace from `linuxsh` — not yet cracked.
- `bcmsh`/`linuxsh` are **console-only** — telnet returns *"Access Denied"*. All diag
  capture must go over the **serial console**.
- `phy diag xeN eyescan` / `state <n>` / `link` / `dump`: not supported by those names on
  this SDK (no output / invalid). `phy diag xeN dsc` is the working SerDes view.

## Capture methodology (tools in `../../tools/`)
- **`icos_capture.py`** — telnet (port 23) driver for the **ICOS CLI** (clean, full-width;
  use for `show ...`). bcmsh is denied over telnet.
- **`ser_diag.py`** — **serial** (`/dev/ttyUSB1`) driver for the **bcmsh** diag shell:
  auto-login, enters bcmsh, **auto-feeds the FASTPATH pager**, **paces input + a
  sacrificial leading CR** to stop the console dropping the first char, and is
  state-aware (detects if already in `BCM.0>`).
- Login: ICOS `admin`/blank → `enable`/blank; bcmsh needs a `y` confirm (blocks mgmt).
