# AS4610-54T — Linux 5.10 own-build NOS (Buildroot / Helix4 / brcm-iproc)

Status (2026-06-13): **5.10.224 boots clean on real hardware as a PURE own-build NOS
(Buildroot glibc+systemd, armhf hard-float, hostname `edgenos-4610`), datapath autostarts,
0% ping both ports.** This is Step 2 of the kernel ladder (Step 1 = 4.19, see
[`KERNEL_419_PORT.md`](KERNEL_419_PORT.md)). The decision was to make the 4610's 5.10 NOS
*ours* (newnos-style, like the AS5610) rather than ride the stock ONL rootfs.

Kernel release string: `5.10.224-OpenNetworkLinux-armhf` (matches newnos/AS5610). Board:
Edgecore AS4610-54T, BCM56340 "Helix4" SoC, dual Cortex-A9, on-die CMICd, ARM/u-boot.

> **Why the AS4610 still needs the out-of-tree BSP on 5.10.** The 5610's newnos is "clean
> vanilla 5.10" only because its P2020 is fully mainline (switch = PCIe device + GPL BDE).
> The AS4610 is ARM Helix4 with the **CPU on the switch die**, so it STILL needs the
> out-of-tree XGS-iProc BSP (the 118-new-file `brcm-iproc` patch) on 5.10. Mainline 5.10 has
> the *generic* iProc plumbing (ARCH_BCM_IPROC, mach-bcm, clk-iproc) but NOT the XGS
> switch-SoC family (no helix4/56340 dts, no `mach-iproc`, no `drivers/soc/bcm/xgs_iproc`).

---

## 1. Artifacts (in `output/kport510/`)

| file | md5 | what |
|------|-----|------|
| `linux-5.10.224/` | — | working kernel tree (pristine 5.10.224 + the patch below, **clean** cgroup build) |
| `arm-accton-as4610-54-r0-510.itb` | `1a712091` | **canonical 5.10 kernel FIT** = cgroup kernel + RTC-disabled DTB + onl-loader-initrd (byte-reproducible from source). This is the **MAIN itb** on the box. |
| `EdgeNOS-4610-510-ownbuild.swi` | `19acad7f` | **pure Buildroot own-build rootfs** (glibc+systemd) + ko510 lockstep with the kernel + datapath autostart — the deployed NOS (§4/§5) |
| `EdgeNOS-4610-510-auto.swi` | `16b205fc` | earlier 5.10 rootfs on the **stock ONL** base (sysvinit) + ko510 — the ONL-base fallback NOS |
| `ko510/*.ko` | — | the 3 GPL BDE/KNET modules built vs this exact kernel (vermagic `5.10.224-OpenNetworkLinux-armhf`) |
| `edgenos-510.its` | — | FIT source (kernel + rtcdis DTB + initrd) for the 5.10 FIT |
| `config-5.10.224-as4610-cgroup` | — | the working 5.10 `.config` (4.19 seed + olddefconfig + cgroups fragment) |

**Productionized build scripts** (all reproducible, run in `edgenos/builder9:1.8-rootless`):
- `nos/kernel/patches/brcm-iproc-5.10.patch` (md5 `5ba61512`, 42958 lines, 158 files) — applies
  `-p1` to pristine `linux-5.10.224`, **0 rejects**.
- `nos/kernel/config/cgroups-systemd.fragment` — the CONFIG_CGROUPS/MEMCG/NS/SECCOMP adds that
  systemd needs (merge into the 4.19-seeded config, then `make olddefconfig`).
- `nos/datapath/sdk-6.5.16-linux5.10-compat.patch` (md5 `cf959129`, 304 lines, 6 files) — the
  SDK 6.5.16 → 5.10 source deltas (all `LINUX_VERSION_CODE`-guarded, 4.x-back-compat). Applies
  `-p1` to `OpenBCM/sdk-6.5.16/src/gpl-modules`, 0 rejects.
- `nos/build-510-fit.sh` — `dtc` DTS→DTB + `gzip` Image + `mkimage` → 5.10 FIT. Byte-reproducible
  (pins `SOURCE_DATE_EPOCH=1717480800`); emits `1a712091`.
- `nos/build-ownbuild-swi.sh` — **the one command.** [1/3] rebuilds ko510 against the *current*
  kernel (the lockstep fix, see §5 fix 5), [2/3] bakes them into the Buildroot squashfs, [3/3]
  wraps the ONL SWI → `EdgeNOS-4610-510-ownbuild.swi`.
- `nos/build-510-installer.sh` — reuses ONL's `installer.sh`/sfx, swaps in the 5.10 FIT + ownbuild
  SWI + explicit `boot-config`, recomputes the loader-initrd offset/size via `pyfit`, repackages
  with `mkshar` → `output/onie-installer-edgenos-510-ownbuild` (the from-scratch / reflash
  installer; ONIE: `onie-nos-install`).
- `nos/datapath/build-bde-510.sh` — cross-builds kernel-bde + user-bde vs the 5.10 tree (knet is
  built directly with `target=linux-iproc`; `build-ownbuild-swi.sh` calls this then does knet).

Pristine kernel source: `OpenNetworkLinux/.../archives/linux-5.10.224.tar.xz`.

---

## 2. How the kernel was produced / how to reproduce the tree

```sh
tar xf .../archives/linux-5.10.224.tar.xz                 # pristine
cd linux-5.10.224
patch -p1 < nos/kernel/patches/brcm-iproc-5.10.patch      # forward-port + all fixes
# config: 4.19 as4610 .config  ->  merge cgroups-systemd.fragment  ->  make olddefconfig
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- clean    # MANDATORY after cgroup config (see §5)
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j"$(nproc)" Image modules
```
Toolchain lives in `edgenos/builder9:1.8-rootless` (`arm-linux-gnueabihf-`). Run via
`docker run -u root:0 -v $EDGE:$EDGE -w <kerndir>`. Clean Image is **13.1 MB** (an incremental
Image after adding MEMCG is ~12.1 MB and is BROKEN — see §5 fix 3).

The patch = (`brcm-iproc-4.19.patch` forward-ported to 5.10) + the §3 fixes. Files reverted to
pristine 5.10 mainline correctly produce no diff entry.

---

## 3. Forward-port 4.19 → 5.10: it was tiny

Applying `brcm-iproc-4.19.patch` to pristine 5.10.224 left only **18 failed hunks / 14 files**
(the 118-file BSP applied clean — new files). Fixes folded into `brcm-iproc-5.10.patch`:

1. **8 Makefile/Kconfig integration rejects** re-applied (same intent as 4.19, drifted anchors —
   `IOP33X`→`IOP32X`, MDIO moved out of `phy/`, the iproc dtb block placement, etc.).
2. **`---help---` → `help` sweep** — 5.10 removed the `---help---` Kconfig token.
3. **`shm.c`** — `device_create_vargs()` was removed in 5.x → `vsnprintf` + `device_create`.
4. **`sdhci-bcm-hr3.c`** — `SDHCI_CLOCK_BASE_SHIFT` removed → local `#define`.
5. **`bcm-helix4.dtsi`** — `#include "skeleton.dtsi"` (removed in 5.x) → inline the root `#address-cells`/`#size-cells`. (Watch ordering: cells must precede subnodes or `dtc` errors
   "Properties must precede subnodes".)

Result: Image + modules (0 err) + `bcm956340.dtb` all build. The RTC-disabled board DTB is the
same flat `nos/kernel/dts/arm-accton-as4610-rtcdis.dts` carried over from 4.19 (the RTC livelock
fix from [`KERNEL_419_PORT.md`](KERNEL_419_PORT.md) §5b still applies — disable `rtc@68`).

---

## 4. The SDK 6.5.16 → 5.10 source deltas (BDE/KNET)

Kept **SDK 6.5.16** (not 6.5.27) for LUBDE ioctl-ABI parity with the existing `bcmd` (which is
kernel-independent and needs no rebuild). The GPL modules need these 5.10 source fixes, all
captured in `sdk-6.5.16-linux5.10-compat.patch` (6 files, every change `LINUX_VERSION_CODE`-guarded
so the 4.x build is unaffected):

- `systems/linux/kernel/modules/shared/gmodule.c` — `struct proc_ops` (≥5.6) for `_gmodule_proc_fops`; `unlocked_ioctl` guard.
- `.../shared/ksal.c` — `ktime_get_real_ts64` / `timespec64` (≥5.0).
- `modules/include/lkm.h` — `#define ioremap_nocache(a,s) ioremap(a,s)` (≥5.6 removed `ioremap_nocache`).
- `systems/bde/linux/kernel/linux_dma.c` — `#undef DMA_MAPPING_ERROR` (5.10 defines it as a macro that clashes).
- `.../bcm-knet/bcm-knet.c` — `#undef DMA_MAPPING_ERROR` + 6× `struct file_operations` → `struct proc_ops`.
- `.../bcm-knet/Makefile` — `$(wildcard …uk-proxy…)` + **propagate `KBUILD_EXTRA_SYMBOLS` to the
  inner kbuild** (5.10 modpost change: without it KNET fails modpost with undefined `lkbde_*`).

Reproduce:
```sh
cd OpenBCM/sdk-6.5.16/src/gpl-modules
patch -p1 < nos/datapath/sdk-6.5.16-linux5.10-compat.patch
# then: nos/datapath/build-bde-510.sh  (kernel-bde + user-bde; knet built directly, see script)
```
All 3 modules build with vermagic `5.10.224-OpenNetworkLinux-armhf` → `output/kport510/ko510/`.

---

## 5. The five own-build last-mile fixes (each caused a distinct boot failure)

The pure Buildroot rootfs (`ownbuild/`, Buildroot 2023.02.9, glibc + systemd, squashfs/xz,
`config/buildroot_defconfig` + `config/overlay` with the datapath + systemd autostart baked in).
Build runs on the HOST as non-root (Buildroot refuses root). Getting it to boot took five fixes:

1. **soft-float SIGILL (init exitcode 0x04).** Cortex-A9's FPU is *optional*, so `BR2_cortex_a9`
   only sets `MAYBE_HAS_VFPV3`; without **`BR2_ARM_ENABLE_VFP=y`** the EABIHF/VFPv3 lines silently
   drop to soft-float EABI, mismatching the hard-float (armhf) kernel + userland → illegal
   instruction in PID 1. Fix = add `BR2_ARM_ENABLE_VFP=y` (verify `FLOAT_ABI="hard"` + EABIHF).
2. **systemd freeze at `/sys/fs/cgroup`.** The 4.19-seeded config (sysvinit ONL) lacks
   `CONFIG_CGROUPS`. Added CGROUPS/MEMCG/BLK_CGROUP/CGROUP_*/CPUSETS/NAMESPACES/*_NS/USER_NS/
   SECCOMP[_FILTER] (= `cgroups-systemd.fragment`) → reached `edgenos-4610 login` + datapath
   autostart = own-build CONCEPT PROVEN.
3. **incremental-Image inconsistency → RCU-stall/panic.** Adding MEMCG changes `struct page` /
   `struct task_struct` layout; an **incremental** `make Image` produced a 12.1 MB Image (vs 13.1
   MB clean — ~1 MB of cgroup code missing) that paniced. Fix = **`make clean`** then rebuild.
   LESSON: after any MEMCG/CGROUPS config change ALWAYS clean-rebuild, never incremental Image.
4. **boot_slot reverting to 4.19 → systemd `clock_gettime` abort.** This u-boot's in-`bootcmd`
   `saveenv` is unreliable, so `boot_slot` kept reverting to the 4.19 main itb. Buildroot glibc
   2.36 uses **64-bit `time_t` (`clock_gettime64`)** which needs kernel **≥5.1**; on 4.19 systemd
   aborts. Fix = make the **clean 5.10 FIT the MAIN itb** (`1a712091`), don't rely on `boot_slot`.
   4.19 main backed up at `sda1:main-419-backup.itb`.
5. **KNET `dma_map_single` Fatal exception.** ko510 built against a *different* kernel config
   loads fine (vermagic matches) but faults in KNET — the MEMCG `struct page` ABI mismatch again.
   Fix = **rebuild ko510 against the cgroup kernel.** This is now baked into
   `nos/build-ownbuild-swi.sh` step [1/3] so the modules are always in lockstep with the kernel.

**Result:** uname `5.10.224`, hostname `edgenos-4610` (no ONL/Debian), datapath autostarts →
`bcmd link 3/54 up` + **ping `10.101.102.1` (xe0 SFP+) 0% + `10.14.1.254` (ge25 copper) 0%**.
Step-2 own-build GOAL ACHIEVED.

---

## 6. Build + deploy: the one command + the installer

**Rebuild the deployable own-build SWI** (after a kernel or rootfs change):
```sh
nos/build-510-fit.sh         # (if the kernel changed) -> clean 5.10 FIT 1a712091
nos/build-ownbuild-swi.sh    # ko510 rebuilt vs the kernel -> baked into squashfs -> SWI 19acad7f
```
`build-ownbuild-swi.sh` guarantees the BDE/KNET modules are in lockstep with the kernel (fix 5),
so the two-step "rebuild kernel → forgot to rebuild modules → KNET fault" trap can't recur.

**Build the from-scratch / reflash installer:**
```sh
nos/build-510-installer.sh   # -> output/onie-installer-edgenos-510-ownbuild
```
On a fresh/reflashed AS4610 from ONIE rescue: `export PATH=$PATH:/sbin:/usr/sbin;
onie-nos-install http://<host>:8080/onie-installer-edgenos-510-ownbuild`. (The `PATH` export
works around ONIE SSH's minimal PATH — `onie-nos-install` calls `reboot`; see
[`KERNEL_419_PORT.md`](KERNEL_419_PORT.md) §10.)

**Deploy a new kernel itb in place** (box already installed), over ONIE-rescue SSH:
```sh
mount /dev/sda1 /mnt/b
wget http://<host>:8080/arm-accton-as4610-54-r0-510.itb -O /mnt/b/arm-accton-as4610-54-r0.itb
sync; /sbin/reboot
```

### Current on-box state (2026-06-13)
- main itb (`/mnt/onl/boot/arm-accton-as4610-54-r0.itb`) = **clean 5.10 FIT** `1a712091`
- `main-419-backup.itb` = the 4.19 main (`c146e335`) — stable fallback
- `boot-config` → `SWI=images:EdgeNOS-4610-510-ownbuild.swi` (the own-build); `boot_slot=A`
- a power-cycle reproduces the autostarting own-build datapath, 0% ping both ports

The recovery toolkit (serial uboot-catch, `run onie_rescue`, ONIE legacy-algo SSH, `panic=10` in
`nos_bootcmd`) is unchanged from [`KERNEL_419_PORT.md`](KERNEL_419_PORT.md) §7 — re-read it before
any risky deploy. The working **4.19 ONL install is the stable fallback** while 5.10 matures.

---

## 7. Key cross-cutting lessons (carry forward up the ladder)

1. **Buildroot Cortex-A9 needs `BR2_ARM_ENABLE_VFP=y`** for hard-float (A9 FPU is optional).
2. **Buildroot glibc 64-bit `time_t` requires kernel ≥5.1** — systemd aborts on 4.19. (This is
   *why* the own-build forced the 5.10 step and can't run on 4.19.)
3. **After a MEMCG/CGROUPS config change: `make clean` the kernel AND rebuild ALL out-of-tree
   modules.** Vermagic matches but `struct page`/`task_struct` layout doesn't → KNET
   `dma_map_single` fault / RCU-stall. (Baked into `build-ownbuild-swi.sh`.)
4. **This u-boot's in-`bootcmd` `saveenv` is unreliable** — make the target kernel the MAIN itb;
   don't rely on `boot_slot` A/B auto-revert. Set `boot_slot` via `fw_setenv` from the OS/ONIE.
5. **The AS4610 needs the out-of-tree XGS-iProc BSP on every kernel** (CPU-on-die) — mainline only
   has generic iProc. Each ladder step = re-floating `brcm-iproc-<ver>.patch`.

---

## 8. Remaining work

1. Kernel ladder: 5.10 → newer LTS when needed (same re-float-the-patch method).
2. AS4610 ARP/neighbor-resolution layer feeding the L3 tables (task #8).
3. Wire `edged`/`bcmd` DMA hooks fully to linux-kernel-bde + verify RX/TX (task #9).
4. Optional: install-test `onie-installer-edgenos-510-ownbuild` end-to-end from scratch (the 419f
   installer was fully install-tested; the 5.10 own-build was validated via in-place deploy).
