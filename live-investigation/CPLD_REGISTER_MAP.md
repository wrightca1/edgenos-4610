# AS4610-54T CPLD register map — ONL driver vs live hardware (VERIFIED)

Cross-check of ONL's `accton_as4610_cpld` (+ `_psu`/`_fan`/`_leds`) register map
against a **live read of the CPLD** (i2c-0 @ 0x30) on the running box. Read-only
(SMBus read_byte_data) via a soft-float `i2cread` tool (the ICOS kernel is
soft-float / no VFP, so the reader had to be built soft-float — see note below).

**Result: the vendor ONL register map matches the silicon byte-for-byte on the
key registers.** Raw dump: [`dumps/cpld_dump_raw.txt`](dumps/cpld_dump_raw.txt).

## Verified registers (CPLD @ 0x30)

| Reg | ONL meaning (driver) | Live value | Decode / cross-check |
|---|---|---|---|
| `0x01` | Product ID | **`0x02`** | `0x02` = ONL `PID_AS4610_54T` → **board self-IDs as 54T** ✅ (exact) |
| `0x11` | **PSU status** (psu.c) + fan status (fan.c) | **`0x07`** | PSU1 present(b0)+pg(b1)=**Operational**; PSU2 present(b2)+**no-pg(b3)=Not powered** — matches `show environment` exactly ✅ |
| `0x02`/`0x03` | SFP+ present/LOS (ports 49–52) | `0x22`/`0x22` | no optics inserted — consistent ✅ |
| `0x21` | QSFP present (STK1/STK2) | `0x00` | no QSFP inserted ✅ |
| `0x2B` | fan PWM (all fans, mask 0xF) | `0x02` | default duty; box is fanless |
| `0x2C`/`0x2D` | fan2 / fan1 RPM | `0x00`/`0x00` | 0 RPM — fanless ("No fans detected") ✅ |
| `0x30`/`0x31` | 7-seg digit 2 / 1 | `0x00`/`0x00` | stack-ID display blank/0 |
| `0x1A` | LED blink control | `0x00` | — |
| `0x32` | SYS/PRI/PSU1/PSU2 LED | `0x89` | active LED state (SYS + PSU LEDs reflecting PS-1 up / PS-2 down) ✅ |
| `0x33` | STK1/2/Fan/PoE/Alarm LED | `0x00` | — |

The two decisive matches:
- **`0x01 = 0x02`** → the CPLD's product-ID register returns exactly the value
  ONL's `enum` maps to **AS4610-54T**.
- **`0x11 = 0x07`** → ONL's PSU bit layout (`present=bit(i*2)`, `pg=bit(i*2+1)`)
  decodes to *PS-1 Operational, PS-2 not-powered* — identical to `show
  environment`. So ONL's PSU/fan status decode is correct on real hardware.

## Conclusion

ONL's `accton_as4610_cpld` register map (PID, PSU/fan status `0x11`, SFP/QSFP
present `0x02`/`0x03`/`0x21`, fan PWM/RPM `0x2B`/`0x2C`/`0x2D`, LEDs
`0x30`–`0x33`) is **authoritative and confirmed** for this board. Nothing
proprietary or hidden in the CPLD path — our NOS's environmental stack reads the
right registers. No reverse-engineering needed; this was confirmation.

## Note: ICOS is soft-float (no VFP) — not a problem for our NOS

The box's `/proc/cpuinfo` shows `Features: half thumb fastmult edsp tls` — **no
`vfp`/`neon`**. The ICOS (Broadcom XLDK) kernel is built **soft-float**, so a
hard-float (`armhf`) binary traps with SIGILL on it. The `i2cread` tool was
therefore built with the **soft-float** toolchain (`arm-linux-gnueabi-gcc`).
This is purely an ICOS-kernel trait: **our ONL kernel is armhf with VFP enabled**,
so our (hard-float) datapath + ONLP binaries run fine — this only affected
running an ad-hoc tool under ICOS.
