# From a 2019 Broadcom BSP to mainline Linux 6.1: rebuilding the NOS on an Edgecore AS4610-54T

*A technical account of taking a white-box enterprise switch off its end-of-life vendor
firmware and onto a modern, self-built, open Network OS — kernel, datapath, and all —
with zero proprietary NOS code.*

---

## TL;DR

We took an **Edgecore AS4610-54T** (Broadcom BCM56340 "Helix4" switch SoC) that shipped
with **Broadcom FASTPATH/ICOS 3.4.3.7 running Linux 4.4.39** (a 2019-vintage Broadcom
BSP kernel, end-of-life since Feb 2022) and replaced the entire stack with **EdgeNOS-4610**:
our own from-scratch Buildroot userland on a **forward-ported mainline Linux 6.1.175 LTS**
(security-maintained through Dec 2027), driving the switch ASIC with an open datapath
daemon over the GPL Broadcom BDE/KNET modules — **no proprietary `switchdrvr`**.

The kernel was modernized one LTS at a time — **4.14 → 4.19 → 5.10 → 5.15 → 6.1** —
re-floating the out-of-tree XGS-iProc BSP at each step and proving the datapath on real
hardware at each rung. The result is installed as the default on both A/B boot slots,
auto-starts the data plane on cold boot, and forwards on both copper and fiber at 0% loss.

| | Before (stock) | After (ours) |
|---|---|---|
| **NOS** | Broadcom FASTPATH/ICOS 3.4.3.7 (Feb 2020) | EdgeNOS-4610 (Buildroot, glibc+systemd) |
| **Kernel** | **Linux 4.4.39-Broadcom** (XLDK-4.2.1, gcc 4.9.3, 2019) | **Linux 6.1.175** (forward-ported XGS-iProc BSP) |
| **Kernel EOL** | 4.4: EOL **Feb 2022** | 6.1: EOL **Dec 2027** |
| **Datapath** | proprietary `switchdrvr` (closed) | `bcmd` (open) + GPL BDE/KNET |
| **Userland** | proprietary FASTPATH image | Buildroot 2023.02.9, systemd |
| **Source posture** | binary blob | GPL + Broadcom source-available, rebuildable |

---

## 1. The hardware

The AS4610-54T is built around the **Broadcom BCM56340 "Helix4"** — a *single-chip
enterprise switch*: the CPU is **on the switch die**, not a separate host over PCIe.

- **CPU:** dual-core ARM **Cortex-A9** @ ~1 GHz, armv7-a **hard-float**, integrated in the
  ASIC's iProc complex.
- **CPU↔switch path:** **on-die CMICd** (memory-mapped; no PCIe/PAXB), driven through the
  GPL BDE.
- **Front panel:** 48× 10/100/1000BASE-T (copper, BCM54282 QSGMII PHYs) + 4× 10G SFP+ +
  2× 20G QSFP+ stacking. The 10G SFP+ uplinks are served by **BCM84758** PHYs.
- **Boot:** u-boot → **ONIE** on flash → NOS on the `/dev/sda` USB-attached storage.
- **System management:** CPLD + I²C tree (PSU, fan, thermal, SFP, LEDs).

This "CPU-on-die" architecture is the crux of the kernel work: **mainline Linux has the
*generic* iProc plumbing (`ARCH_BCM_IPROC`, `mach-bcm`, `clk-iproc`) but not the XGS
switch-SoC family** — no Helix4/56340 device tree, no `mach-iproc`, no
`drivers/soc/bcm/xgs_iproc`. That entire BSP lives out-of-tree and must be carried (and
re-floated) on every kernel we build.

---

## 2. The starting point: what ICOS actually was

The device self-identified through its on-box VPD (`/mnt/application/fastpath.vpd`):

```
Operational Code Image File Name - FastPath-ICOS-esw-as4610-iproc_as4610-XL4XR-CNTRF-...
Rel 3, Ver 4, Maint Lev 3, Bld No 7
Timestamp - Fri Feb 28 17:27:08 EST 2020
```

So the stock NOS was **Broadcom FASTPATH**, branded **ICOS, version 3.4.3.7**, built Feb
2020. Carving its operational image out of the flash (`image1`, a u-boot multi-file
uImage) and decompressing the kernel yields the banner:

```
Linux version 4.4.39-Broadcom XLDK-4.2.1-ea1c3a24 (gcc 4.9.3, Buildroot 2015.11.1)
#1 SMP PREEMPT Wed Mar 27 18:14:32 EDT 2019
```

**That is the headline problem:** the switch was running a **Linux 4.4.39** kernel from a
2015-era Buildroot toolchain and a 2019 Broadcom XLDK BSP. The 4.4 LTS series reached
**end-of-life in February 2022** — no more upstream security fixes. The userland, init,
and datapath were all closed FASTPATH code. There was no path to a current kernel without
owning the stack.

> **Provenance note.** Everything here came from hardware we own: the live stock NOS was
> inspected read-only, and a full bit-for-bit backup of the original flash (u-boot, ONIE,
> both FASTPATH images, config) was taken first and verified restorable, so the box can
> always return to factory ICOS.

---

## 3. Strategy: own the stack, modernize the kernel incrementally

Two decisions framed the work:

1. **Build our own NOS** (like we did for the sister AS5610), rather than ride a
   distribution. The deliverable is *our* kernel + *our* modules + *our* userland + *our*
   image format.
2. **Climb the kernel one LTS at a time.** A 4.4 → 6.1 jump in one shot against an
   out-of-tree switch-SoC BSP is a recipe for an undebuggable pile of rejects. Instead:
   **4.14 → 4.19 → 5.10 → 5.15 → 6.1**, re-floating the BSP patch and proving the datapath
   on real hardware at every rung. Each rung is a small, reviewable delta.

ONL (Open Network Linux) already supported this board at **4.14**, which gave us a known
DTS + ONLP platform drivers to start from — so the campaign began at 4.14 and climbed.

---

## 4. Driving the silicon without the proprietary driver

Replacing FASTPATH means replacing `switchdrvr` — the closed daemon that programs the
ASIC. Our data plane is:

- **`bcmd`** — a userspace daemon built from the **OpenBCM SDK 6.5.16** (source-available).
  It runs the SDK's `init all` / `init bcm` (reading `config.bcm`), brings up the GE/XE
  ports and the BCM84758 SFP+ PHYs, and then implements CPU-punt, per-port VLAN isolation,
  and **L3 hardware forwarding** (it mirrors the Linux FIB — including OSPF/BGP routes from
  Quagga — into the ASIC's L3 tables via netlink, with local-IP CPU punt and connected-route
  glean/ARP).
- **GPL BDE/KNET kernel modules** (`linux-kernel-bde`, `linux-user-bde`, `linux-bcm-knet`)
  from the SDK's `gpl-modules`. These are the only kernel components beyond the BSP, and
  they enumerate the on-die CMIC, provide the coherent DMA pool, and carry packets between
  the chip and the Linux netdevs.

A deliberate design choice that paid off across the whole ladder: **freeze the SDK at
6.5.16.** Because the LUBDE ioctl ABI between `bcmd` and the BDE stays fixed, `bcmd` is
**kernel-independent** — we rebuild only the three GPL modules per kernel (vermagic must
match), never `bcmd`. That kept the per-rung work bounded to "re-float the BSP + reshim
the modules."

---

## 5. The kernel ladder, rung by rung

The out-of-tree BSP is a ~118-new-file patch (`drivers/soc/bcm/xgs_iproc`,
`arch/arm/mach-iproc`, the Helix4 DTS, serdes/MDIO/SMBus/flash/USB drivers, …). At each
LTS we applied the previous rung's patch to the new pristine tree, fixed the rejects and
the API breaks, and rebuilt. The canonical patches are validated by **applying to a fresh
pristine tree *and building*** — not just by reject count (a lesson learned the hard way;
see §5.6).

### 5.1 — 4.14 → 4.19.81
Small delta. Notable fixes folded in: `sp805_wdt` and `spi-bcm-qspi` reverted to mainline
(the 4.14 hunks were obsolete/half-applying — the watchdog one NULL-derefed PID 1 on every
boot); `board_bu.c` DMA-pool API change; USB-PHY decl scope; and the iProc i2c
clock-frequency enum.

**The signature 4.19 bug — the RTC autostart wedge.** After the port, the box pinged but
never reached `sshd`/`getty`. Root cause: the board's dead-battery RTC made the 4.19 RTC
core perpetually re-arm an expired alarm; each pass was a **muxed I²C read holding the
i2c1 bus lock**, which starved `onlpd` before login. (4.14 tolerated it only because
Edgecore had commented out a per-transaction `msleep(1)` that the forward-port re-added.)
**Fix: disable the dead RTC in the DTB** — driver never probes, lock never held, boot
completes; NTP sets the clock. Result: clean boot, datapath works, 0% ping.

### 5.2 — 4.19 → 5.10.224
Tiny BSP delta (the 118-file core applied clean). Fixes: a `---help---`→`help` Kconfig
sweep (token removed in 5.x); `shm.c` `device_create_vargs()` removed → `vsnprintf` +
`device_create`; `sdhci` `SDHCI_CLOCK_BASE_SHIFT` removed → local `#define`;
`bcm-helix4.dtsi` `#include "skeleton.dtsi"` (gone in 5.x) → inline root cells. SDK 6.5.16
modules needed `LINUX_VERSION_CODE`-guarded shims: `proc_ops` (5.6), `ktime_get_real_ts64`,
`ioremap_nocache`→`ioremap`, a `DMA_MAPPING_ERROR` redefine, and KNET modpost
`KBUILD_EXTRA_SYMBOLS` propagation.

### 5.3 — the own-build (Buildroot) NOS, on 5.10
Having proven 5.10 on hardware, we replaced the ONL base rootfs with **our own Buildroot
2023.02.9 image** (internal glibc/GCC-12 toolchain, **systemd**, squashfs, hostname
`edgenos-4610`). Five last-mile fixes earned their keep:
1. **`BR2_ARM_ENABLE_VFP=y`** — Cortex-A9's FPU is *optional*, so without it the EABIHF
   build silently fell back to soft-float and PID 1 took a SIGILL. Hard-float is mandatory.
2. **CGROUPS/MEMCG/NS/SECCOMP** in the kernel config — systemd freezes mounting
   `/sys/fs/cgroup` without them.
3. **Clean kernel rebuild after enabling MEMCG** — MEMCG changes `struct page`/
   `task_struct` layout; an *incremental* `make Image` produced an inconsistent kernel
   that paniced. (Corollary: rebuild *all* out-of-tree modules too — the KNET
   `dma_map_single` faulted until ko was rebuilt against the cgroup kernel.)
4. Buildroot's **glibc 2.36 uses 64-bit `time_t`** (`clock_gettime64`), which needs kernel
   **≥ 5.1** — systemd aborts on 4.19. (This is *why* the own-build forced the 5.10 step.)
5. This u-boot's in-`bootcmd` `saveenv` is unreliable — make the target kernel the **main**
   boot itb; don't depend on the A/B auto-revert.

### 5.4 — 5.10 → 5.15.209 (matches our AS5610)
The disciplined rung: 6 failed hunks, ~7 small deltas total. The only C-level breaks were
`ehci-platform.c` (`struct ehci_regs` reorganized into unions — the BSP's `reserved4[6]`
write became `brcm_insnreg[1]`, same 0x84 INSNREG01 offset) and an SDK `ksal.c` fix
(`MAX_USER_RT_PRIO` removed → the old `SCHED_YIELD` path; version-guarded to `yield()`).
SDK stayed frozen; `bcmd` untouched. HW-validated: copper 0%, fiber 0%.

### 5.5 — 5.15 → 6.1.175 (first 6.x)
Bigger delta, as expected crossing into 6.x — but still ~10 fixes:
- **Kernel C:** `devm_gpio_free()` was **removed** in 6.x (the devm-managed GPIO auto-frees;
  drop the explicit call, ×2 in `phy-xgs-iproc.c`); `ehci-platform.c`
  `bcm_iproc_insnreg01`→`brcm_insnreg[1]` (6.1 mainline added its own iProc EHCI support,
  so the old alias is gone).
- **SDK/KNET (4 shims, version-guarded):** `dev->dev_addr` became `const` (5.17) →
  `eth_hw_addr_set()`; `pci_set_dma_mask` removed → `dma_set_mask()`; `netif_napi_add`
  dropped its `weight` argument (6.1) → `netif_napi_add_weight()`.
- **The runtime one that only a hardware test catches:** the kernel built and the modules
  loaded (vermagic matched), but `linux-kernel-bde`'s `iproc_cmicd_probe()` **NULL-derefed
  at module load**. In 6.x, DT platform IRQs are **no longer kept in the resource table**,
  so `platform_get_resource(pdev, IORESOURCE_IRQ, 0)` returns NULL and `irqres->start`
  faults. Fix: `platform_get_irq(pdev, 0)`. With that, the BDE init completes, the CMIC is
  detected (`type 20000180`), and the datapath comes up — **copper + fiber both 0% loss.**

### 5.6 — a patch-hygiene lesson worth recording
Our "clean" canonical patches were generated with `diff -uprN -X dontdiff`. We discovered
`diff -rN` **silently drops some new files under `include/` during full-tree recursion** —
so a patch could apply to a pristine tree with **zero rejects yet fail to build** (it was
missing five BSP headers). The fix: append the missing headers explicitly, and **verify
every canonical patch by applying to a fresh pristine tree *and compiling it*** — reject
count is necessary but not sufficient.

---

## 6. Image format, install, and the A/B dual-slot scheme

EdgeNOS boots the ONL way, which we kept (and demystified):

- A **loader FIT** (`.itb`: gzipped kernel `Image` + the RTC-disabled board DTB + an
  ONL loader-initrd) lives on the boot partition (`/dev/sda1`). u-boot's `nos_bootcmd`
  loads it.
- The FIT's loader-initrd reads `boot-config` and pulls a **SWI** (a zip of the rootfs
  squashfs + manifest) from the images partition (`/dev/sda3`), extracting it into RAM and
  `switch_root`-ing into it. systemd then auto-starts the datapath (`bcmd.service`).
- **A/B slots:** the u-boot `boot_slot` variable selects between two kernel itbs
  (`...-r0.itb` = slot A/main, `...-r0-test.itb` = slot B/test). The A/B split is the
  *kernel only*; one `boot-config` selects the rootfs SWI for both. `boot_slot` is set
  reliably from the OS via `fw_setenv` (the in-`bootcmd` `saveenv` is the unreliable one).

**ONIE installers, demystified.** An "ONIE NOS installer" is just a **self-extracting
shell archive** (a `.shar` built by ONL's `mkshar` from `sfx.sh.in`): a shell prefix +
a zip payload containing `installer.sh`, the FIT, the SWI, and `boot-config`. The one real
trap is that `installer.sh` carves the loader-initrd out of the FIT at install time with
`dd`, using an **offset/size that is specific to each FIT** — recompute them per build with
`pyfit` (size = last − offset + 1) or the install boots garbage. (A bare `.swi` is *not*
ONIE-installable; it must be wrapped.)

**6.1 is installed as the production default on BOTH slots**, with the previous 5.10 and
4.19 kernels preserved as rollback itbs on disk. Each slot was independently cold-booted
and verified: 6.1.175 to login, baked modules auto-load, `bcmd` auto-starts, **both copper
`ge25` and fiber `xe0` ping at 0% loss with zero manual steps.**

---

## 7. A representative "depth" detail: the port LEDs

A small but illustrative example of building a NOS bottom-up: the SFP+ port LEDs came up
**solid-on**. Cause — `bcmd` brought up forwarding but never programmed the chip's on-die
**LED microcontroller (an M0)**, so its external latches powered up all-lit. (The *system*
LEDs — SYS/PSU/fan — are CPLD-driven via ONLP and were fine.) ICOS loads an LED program,
but it's compiled into the closed `switchdrvr` binary, not a loadable file. The clean
answer was open-source: **OpenMDK ships a 216-byte M0 LED program for the Helix4 GE-family
reference board** (`board/xgsled/sdk53344_ref.c`, Broadcom Switch-APIs license). We load it
in `bcmd` (`led prog … ; led auto on ; led start`) so linkscan drives link/activity onto
the LEDs. This is the pattern throughout: each piece the proprietary NOS did silently, we
re-implement from open sources, explicitly.

---

## 8. Licensing posture

The result is fully **distributable**, though source-available rather than pure-OSI:

- **GPL:** the Linux kernel + the BSP patch, the BDE/KNET modules, Buildroot, Quagga.
- **Broadcom source-available** (grant explicitly allows distribute + derivative +
  sublicense): the OpenBCM SDK, OpenMDK, and the BCM84758 PHY firmware (kept as a
  `request_firmware` blob from Broadcom's own public repo).

We keep the Broadcom notices/LICENSE, label per-component, and GPL-comply the kernel and
modules. The proprietary stock ICOS images are retained privately as a verified-restorable
backup only.

---

## 9. Result

A decade-old enterprise switch, abandoned by its vendor on a 2019 Linux 4.4 BSP, now runs a
**self-built, open, systemd-based NOS on mainline-derived Linux 6.1.175** — security-
maintained through **December 2027** — with a fully open data plane forwarding line-rate on
copper and fiber, installed redundantly across both boot slots and auto-starting
unattended. The kernel is modernizable further by the same one-LTS-at-a-time method
(6.6 and 6.1 share a Dec-2027 EOL; 6.12+ goes further).

The quiet thesis: *"this hardware is end-of-life" almost always means the vendor is done
with it, not the silicon.* Because the OS is open and Linux-native, the box gets to keep up
with Linux.

---

*Platform: Edgecore AS4610-54T (BCM56340 Helix4, dual Cortex-A9 armv7-hf, on-die CMICd).
Stock: Broadcom FASTPATH/ICOS 3.4.3.7 on Linux 4.4.39-Broadcom (2019/2020).
Now: EdgeNOS-4610 (Buildroot glibc+systemd) on forward-ported Linux 6.1.175 LTS.
Kernel ladder: 4.14 → 4.19 → 5.10 → 5.15 → 6.1, hardware-proven at every rung from 4.19 up.*
