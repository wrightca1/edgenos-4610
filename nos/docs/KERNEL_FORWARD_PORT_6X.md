# Kernel Forward-Port: ONL 4.14 → mainline 6.x

Phase 3. We start on ONL's proven iProc **4.14** to get the board booting and
forwarding fast; this is the roadmap to a modern mainline LTS kernel ("the latest
Linux we can"), which was the original goal.

## Why it's feasible (not a from-scratch port)

The XGS-iProc switch-SoC family **is** upstream — Helix4's close sibling
**Hurricane 2** has `ARCH_BCM_HR2` (BCM5334x, dual Cortex-A9) and `bcm-hr2.dtsi`
in mainline, and Allied Telesis has been upstreaming this family. The shared
peripheral IP we need is already mainline:

| Block | Mainline driver | Our DTS node |
|---|---|---|
| Chip Common A GPIO | `gpio-xgs-iproc` (`CONFIG_GPIO_BCM_XGS_IPROC`, 2019) | `gpio_cca` |
| I²C | `i2c-bcm-iproc` | `i2c0`, `i2c1` |
| Pinctrl, GIC, L2C, global timer | `ARCH_BCM_IPROC` base | — |
| Watchdog | `bcm_iproc_wdt` family | `iproc_wdt` |
| USB EHCI + PHY | iProc USB | `ehci0`, `usbphy0` |

So Helix4 is **not in mainline**, but its SoC architecture and most peripherals
are. The job is *adaptation*, not invention.

## What has to be produced

1. **`bcm-helix4.dtsi` for 6.x** — port ONL's `dts/bcm-helix4.dtsi` to mainline
   bindings, using `bcm-hr2.dtsi` as the template (same CPU/GIC/timer/iProc
   layout). Re-base the board DTS (`arm-accton-as4610.dts`) on it.
2. **Machine/SoC enablement** — confirm/add a Helix4 entry under
   `ARCH_BCM_IPROC` (likely reuse HR2's machine glue: SMP, L2C, twd timer, clk).
3. **GMAC (mgmt port)** — the on-die `gmac0` (SGMII). Check which mainline driver
   matches (`bgmac` / iProc AMAC vs the ONL gmac); may need a DT compatible tweak.
4. **Gap-fill** — anything ONL patched that isn't upstream: clocks, pinmux tables,
   any board quirks. Diff ONL's `4.14-lts` iProc patch set
   (`OpenNetworkLinux/packages/base/any/kernels/4.14-lts/.../patches/`) against
   mainline to enumerate the delta.
5. **BDE/CMICd module rebuild** — the switch datapath module is out-of-tree
   regardless of kernel; rebuild it against 6.x headers (API churn: `class_create`,
   `proc_ops`, DMA-API, `ioremap` changes). This is the usual SDK-module porting.

## Method

1. Pick a target: **latest mainline LTS** (6.6 or 6.12 — long support).
2. Boot a generic HR2/iProc 6.x kernel under qemu or on-board to validate the
   base SoC support, then layer the Helix4 DTS.
3. Bisect bring-up: console (UART) → timers/SMP → I²C/GPIO (sensors via ONLP) →
   GMAC (mgmt) → USB (rootfs) → CMICd/BDE (datapath).
4. Keep 4.14 as the reference oracle: anything that works there but not on 6.x is
   a concrete delta to chase.

## Risk / effort

- **Low-risk, mostly mechanical:** DTS port, I²C/GPIO/UART (drivers upstream).
- **Medium:** GMAC mgmt link, clk/pinctrl gaps, SMP/timer machine glue if HR2's
  doesn't cover Helix4 verbatim.
- **Bounded but real:** BDE/CMICd module API churn for 6.x.

No item requires reverse-engineering — it's kernel porting against upstreamed IP.
Defer until phase 1+2 (board boots + forwards on 4.14) are done.
