# EdgeNOS-4610 kernel — incremental modernization

Incremental path to our own NOS on a newer kernel. Decided 2026-06-11.

## Strategy: foundation first, then climb the kernel ladder
The Helix4/iProc platform is **not in mainline** — it's a single 32,416-line
out-of-tree Broadcom patch (`brcm-iproc-4.14.patch`) spanning ~15 subsystems
(arch/arm/mach-iproc, drivers/soc/bcm/xgs_iproc CMIC, clk/bcm, pci/host, net/phy,
usb-gadget, dts…). A single 4.14→5.10 leap = forward-porting all of it across 3
major versions — a multi-week project. So we go incrementally:

- **Step 0 — Foundation (current): custom image on the proven 4.14 kernel.**
  Extend the ONL build (it already carries the working Helix4 4.14 kernel) with
  (a) a kernel-config patch and (b) our datapath baked into the rootfs. Delivers
  **persistence + ECMP + IPv6** now, decoupled from the kernel port.
- **Step 1 — 4.14 → 4.19 LTS. ✅ DONE (2026-06-12).** Forward-ported the iProc patch one LTS;
  4.19.81 boots clean on real HW, datapath works (`init all` → links → 0% ping), autostart wedge
  fixed. Canonical patch: `patches/brcm-iproc-4.19.patch` (applies `-p1` to pristine
  `linux-4.19.81`, 0 rejects). Companion DTB RTC-disable is in `dts/arm-accton-as4610.dts`.
  **Full writeup + root-cause + recovery toolkit: `../../docs/KERNEL_419_PORT.md`.** Fixes:
  sp805_wdt/spi-bcm-qspi reverted to mainline, board_bu coherent-pool, usb-phy decl scope,
  iproc-smbus clock-freq enum, and **disabling the dead-battery RTC** (its perpetual alarm re-arm
  held the muxed i2c lock and wedged onlpd before sshd/getty). SDK 6.5.16 BDE/KNET still apply.
- **Step 2 — 4.19 → 5.10 LTS. ✅ DONE (2026-06-12), incl. a PURE own-build NOS.** Forward-ported
  the iProc patch to 5.10.224 (`patches/brcm-iproc-5.10.patch`, 0 rejects); **kept SDK 6.5.16** (not
  6.5.27) for LUBDE ioctl-ABI parity so `bcmd` needs no rebuild — the GPL modules get 5.10 source
  shims in `../datapath/sdk-6.5.16-linux5.10-compat.patch`. Boots clean on real HW, datapath 0% ping
  both ports. Then built our **own** Buildroot rootfs (glibc+systemd, hostname `edgenos-4610`) — not
  the ONL base. Reproducible builders: `../build-510-fit.sh`, `../build-ownbuild-swi.sh` (rebuilds
  ko510 in lockstep), `../build-510-installer.sh`. **Full writeup: `../../docs/KERNEL_510_OWNBUILD.md`.**
- **Step 3 — 5.10 → 5.15 LTS (matches the 5610/newnos). ✅ DONE + PROVEN ON HW (2026-06-13).** The disciplined
  rung: `brcm-iproc-5.10.patch` re-floated onto pristine 5.15.209 with only 6 failed hunks; total 7
  small deltas (5 Makefile/Kconfig integration + 1 BSP C `ehci-platform.c reserved4[6]`→`brcm_insnreg01`
  + 1 SDK C `ksal.c SCHED_YIELD`/`MAX_USER_RT_PRIO`). Canonical `patches/brcm-iproc-5.15.patch`
  (0 rejects on pristine) + `../datapath/sdk-6.5.16-linux5.15-compat.patch` (superset of the 5.10
  one — `ksal` fix is back-compatible). Kernel `Image` + all modules + the BDE/KNET trio build clean,
  vermagic `5.15.209-OpenNetworkLinux-armhf`; **SDK frozen → `bcmd` unchanged.**
  **HW-validated:** booted the 5.15 FIT from the A/B test slot (`boot_slot=B`; 5.10 left in slot A as
  the fallback) — 5.15.209 boots clean to login (no panic/oops), `ko515` load, CMIC `type 20000180`,
  `bcmd` active, **copper `ge25` ping 10.14.1.254 = 0% loss.** (SFP+ `xe0` was peer-gated: far-side
  router rebooting, our carrier up — not a kernel issue.) Slot deploy done entirely from the live OS:
  write the FIT to the inactive `sda1` test slot + `fw_setenv boot_slot B` (OS `fw_setenv` persists;
  the in-`bootcmd` `saveenv` is the unreliable one). **No ONIE needed** for slot updates — ONIE is
  only for full `onie-nos-install` or unbootable recovery. Build scripts: `../build-515-fit.sh`,
  `../datapath/build-bde-515.sh`. Still to do for a deployable 5.15 NOS: a 5.15 own-build SWI
  (`ko515` baked) + installer, mirroring the 5.10 ones.
- **Step 4 — 5.15 → 6.1 LTS (first 6.x). ✅ BUILD DONE (2026-06-13).** Re-floated `brcm-iproc-5.15.patch`
  onto pristine 6.1.175 — only 6 failed hunks; ~10 total deltas: 3 Makefile/Kconfig integration (dts
  Makefile board DTB, broadcom Makefile APM objs, bgmac.c ethtool add-on) + 6.x C breaks
  (`devm_gpio_free` removed → drop the explicit free, ×2 in phy-xgs-iproc.c; `ehci-platform.c`
  `bcm_iproc_insnreg01`→`brcm_insnreg[1]` since 6.1 mainline added its own iProc EHCI support) + the
  4 SDK/KNET shims (`dev->dev_addr` const@5.17 → `eth_hw_addr_set` ×2; `pci_set_dma_mask` removed →
  `dma_set_mask`; `netif_napi_add` dropped weight@6.1 → `netif_napi_add_weight`). Canonical
  `patches/brcm-iproc-6.1.patch` (0 rejects on pristine, build-verified) +
  `../datapath/sdk-6.5.16-linux6.1-compat.patch` (superset of 5.15's). Kernel `Image` (14.2MB) +
  modules + BDE/KNET trio build clean, vermagic `6.1.175-OpenNetworkLinux-armhf`; **SDK frozen →
  `bcmd` unchanged.** Build scripts: `../build-61-fit.sh`, `../datapath/build-bde-61.sh`.
  **HW: 6.1.175 boots clean to login on real hardware** (twice). Datapath needed one more 6.x fix
  found at *runtime*: `linux-kernel-bde` `iproc_cmicd_probe()` NULL-derefed at load —
  `platform_get_resource(pldev, IORESOURCE_IRQ, 0)` returns NULL in 6.x (DT platform IRQs are no
  longer in the resource table) so `irqres->start` faults; fixed with `platform_get_irq(pldev, 0)`
  (in `sdk-6.5.16-linux6.1-compat.patch`, version-guarded). **✅ HW-VALIDATED (2026-06-14):** after a
  power-cycle, 6.1.175 cold-boots clean, the fixed `ko61` loads (CMIC detected `type 20000180`, BDE
  init completes — no oops), `bcmd` active, **copper `ge25` ping 10.14.1.254 = 0% loss.** The first
  6.x rung is fully proven on hardware. (`class_create` owner-drop is @6.4, next rung's concern.)
  NOTE: rebooting *from* a degraded state (an oopsed module still loaded) can wedge on shutdown
  ("watchdog did not stop!" + busy loop device) → needs a physical power-cycle; a clean cold boot is
  fine.
  NOTE — patch-gen fix: `diff -rN` silently drops new files under `include/` during full-tree
  recursion, so the canonical patches now explicitly append the 5 BSP `include/linux/` headers and are
  **build-verified from pristine** (not just reject-checked). This also fixed the 5.15 patch, which
  was missing those headers.
- **Step 5+ — 6.1 → newer LTS** when needed (`class_create` owner-drop @6.4; `strlcpy` removed @6.8;
  more folio/mm churn).

Fallback at every rung: serial console + ICOS/known-good image rollback (proven).
Licensing for distribution: all components distributable — see memory
`reference_edgenos_licensing` (GPL kernel/modules/quagga + Broadcom source-available SDK/fw).

## Step 0 kernel-config change (applied to the ONL 4.14 source config)
File: `OpenNetworkLinux/packages/base/any/kernels/4.14-lts/configs/armhf-iproc-all/armhf-iproc-all.config`
```
- # CONFIG_IP_ROUTE_MULTIPATH is not set
+ CONFIG_IP_ROUTE_MULTIPATH=y          # activates bcmd's ECMP path (FIB multipath)
- # CONFIG_IPV6_MULTIPLE_TABLES is not set
+ CONFIG_IPV6_MULTIPLE_TABLES=y        # IPv6 policy routing (toward IPv6 L3)
```
(IPv6 forwarding itself is a runtime sysctl; CONFIG_IPV6=y was already set.)
`.config.orig` kept alongside. Rebuild the kernel package, then the image.

## dts/
`arm-accton-as4610.dts` — the AS4610-54 board DTS (from ONL 4.14, Cumulus 2015),
includes `bcm-helix4.dtsi`. Staged here as the base to forward-port at each kernel hop.

## Remaining Step-0 work
1. ONL package that bakes our datapath into the rootfs: bcmd + the 3 .ko +
   config.bcm + quagga (zebra-arm/ospfd-arm) + the `rootfs-overlay/` config files +
   the 4 systemd units (enabled). [see ../rootfs-overlay/]
2. `nos/build.sh` → `make armhf` (long) → custom SWI.
3. ONIE-install → validate datapath (copper + SFP+ + OSPF) survives reboot.
