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
- **Step 1 — 4.14 → 4.19 LTS.** Forward-port the iProc patch one LTS (small delta;
  SDK 6.5.16 BDE/KNET modules still apply).
- **Step 2 — 4.19 → 5.10 LTS** (matches the 5610). Bigger delta; switch BDE/KNET to
  SDK 6.5.27 (its module guards reach 5.5+). Some iProc bits (clk-iproc, pcie-iproc,
  mach-bcm, usb) are mainline by 5.10, shrinking the port to the Helix4-switch parts.

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
