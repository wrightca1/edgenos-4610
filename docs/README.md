# EdgeNOS-4610 documentation

EdgeNOS is an open Network OS for the **Edgecore AS4610-54T** (BCM56340 "Helix4"
switch SoC, CPU-on-die, ARM Cortex-A9, ONIE/u-boot). These docs are the practical,
hard-won kind — what the upstream/vendor docs leave out.

## Start here

| doc | read it when you want to… |
|-----|---------------------------|
| **[BUILD_AND_DEPLOY.md](BUILD_AND_DEPLOY.md)** | understand the whole pipeline (kernel → modules → SWI → installer), the serial toolkit, and how we verify hardware. **Best entry point.** |
| **[ONIE_IMAGE_GUIDE.md](ONIE_IMAGE_GUIDE.md)** | build and install a from-scratch ONIE image. Demystifies what an ONIE installer actually is (a self-extracting shar) and the initrd-offset gotcha. |
| **[AB_DUAL_SLOT_GUIDE.md](AB_DUAL_SLOT_GUIDE.md)** | understand the A/B boot slots and update a slot in-place (no ONIE), plus recovery. |
| **[KERNEL_510_OWNBUILD.md](KERNEL_510_OWNBUILD.md)** | the 5.10 own-build (Buildroot) details + the 5 own-build fixes. |
| **[KERNEL_419_PORT.md](KERNEL_419_PORT.md)** | the 4.19 forward-port + the RTC-wedge root cause + recovery toolkit. |
| **[../nos/kernel/README.md](../nos/kernel/README.md)** | the kernel ladder status: 4.14 → 4.19 → 5.10 → 5.15 → **6.1** (current default). |

## Current state (2026-06-14)

- **Production kernel: Linux 6.1.175** (EOL Dec 2027), installed as the default on
  **both** A/B slots, datapath autostarts on cold boot, **copper + fiber both 0% loss**.
- Kernel ladder all build; **4.19 / 5.10 / 5.15 / 6.1 are hardware-proven.**
- Rollback kernels kept on the box (`main-510-backup.itb`, `main-419-backup.itb`).

## The 60-second mental model

- The CPU is **on the switch die**, so the kernel carries an out-of-tree BSP
  (`nos/kernel/patches/brcm-iproc-<ver>.patch`). Each LTS bump = re-float that patch.
- The datapath is a **userspace daemon `bcmd`** + GPL **BDE/KNET** kernel modules.
  The modules are vermagic-tied to the kernel; `bcmd` is kernel-independent.
- The box boots a **kernel FIT** (`.itb` on `/dev/sda1`) chosen by u-boot `boot_slot`
  (A/B), which loads a **rootfs SWI** (`.swi` on `/dev/sda3`) named by `boot-config`.
- Build artifacts (`output/`, `ownbuild/build/`) are gitignored; only source is tracked.

## Repos

- Code + docs + firmware-as-blobs: GitHub `wrightca1/edgenos-4610` (this repo).
- Proprietary ICOS backup images: private GitLab (never public).
