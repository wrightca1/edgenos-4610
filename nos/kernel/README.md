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
- **Step 3 — 5.10 → 5.15 LTS (matches the 5610/newnos). ✅ BUILD DONE (2026-06-13).** The disciplined
  rung: `brcm-iproc-5.10.patch` re-floated onto pristine 5.15.209 with only 6 failed hunks; total 7
  small deltas (5 Makefile/Kconfig integration + 1 BSP C `ehci-platform.c reserved4[6]`→`brcm_insnreg01`
  + 1 SDK C `ksal.c SCHED_YIELD`/`MAX_USER_RT_PRIO`). Canonical `patches/brcm-iproc-5.15.patch`
  (0 rejects on pristine) + `../datapath/sdk-6.5.16-linux5.15-compat.patch` (superset of the 5.10
  one — `ksal` fix is back-compatible). Kernel `Image` + all modules + the BDE/KNET trio build clean,
  vermagic `5.15.209-OpenNetworkLinux-armhf`; **SDK frozen → `bcmd` unchanged.** HW boot test pending.
- **Step 4+ — 5.15 → newer LTS** when needed (same re-float method; 6.x is a bigger delta —
  `class_create` owner-drop @6.4, mm folios @5.16+).

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
