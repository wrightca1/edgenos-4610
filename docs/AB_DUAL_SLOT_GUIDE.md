# The AS4610 A/B dual-slot boot scheme (how it works + how to use it)

How EdgeNOS boots on the AS4610, how the two boot "slots" work, and the exact,
proven procedure to install an image on **both slots** and verify each one — all
from the running OS, **without ONIE**.

---

## TL;DR

- There are **two kernel slots**, A and B, selected by the u-boot variable
  `boot_slot`. Slot A boots `arm-accton-as4610-54-r0.itb`; slot B boots
  `arm-accton-as4610-54-r0-test.itb`. Both live on the boot partition (`/dev/sda1`).
- There is **one** root filesystem image (`boot-config` → one `.swi` on `/dev/sda3`).
  The A/B split is the **kernel only**, not the rootfs.
- You can update either slot **from the running OS** — mount the partition, copy the
  FIT, set `boot_slot` with `fw_setenv`. **ONIE is not required** for slot updates.
- To make the box boot the same image no matter which slot: put the FIT on **both**
  itb names and point `boot-config` at the SWI.

---

## 1. The boot chain, end to end

```
power on
  └─ u-boot runs `bootcmd` → `nos_bootcmd`
       ├─ if boot_slot == B:  onl_itb = arm-accton-as4610-54-r0-test.itb   (slot B)
       │                      (also tries: setenv boot_slot A; saveenv  <- self-revert)
       └─ else:               onl_itb = arm-accton-as4610-54-r0.itb        (slot A)
     ext2load usb 0:1 $onl_itb ; bootm   <- loads the FIT from /dev/sda1
  └─ FIT = kernel Image + board DTB + onl-loader-initrd
       └─ loader-initrd reads /dev/sda1:boot-config  → "SWI=images:<file>.swi"
            └─ finds <file>.swi on the images partition (/dev/sda3)
                 └─ extracts rootfs squashfs into RAM, switch_root into it
                      └─ systemd → bcmd.service (datapath autostart)
```

The exact `nos_bootcmd` on this box:

```
setenv onl_loadaddr 0x70000000; setenv onl_platform arm-accton-as4610-54-r0;
setenv onl_itb arm-accton-as4610-54-r0.itb;
if test ${boot_slot} = B; then setenv boot_slot A; saveenv;
   setenv onl_itb arm-accton-as4610-54-r0-test.itb; fi;
setenv bootargs console=$consoledev,$baudrate onl_platform=$onl_platform coherent_pool=16M panic=10;
usb start; usbiddev; ext2load usb 0:1 $onl_loadaddr $onl_itb; bootm $onl_loadaddr#$onl_platform
```

> **Why "usb"?** The eMMC/flash is enumerated through the iProc USB controller, so
> u-boot addresses the disk as `usb 0:1` (= `/dev/sda1`, the boot partition).

---

## 2. Partition map (what lives where)

| device | role | holds |
|--------|------|-------|
| `/dev/sda1` | **boot partition** (ext2) | the kernel FITs (`*.itb`) **and** `boot-config` |
| `/dev/sda2` | ONL config | (small) |
| `/dev/sda3` | **images partition** = `images:` in boot-config | the `.swi` files |
| `/dev/sda4` | data | logs, persistent data |

> **Gotcha:** the running Buildroot rootfs does **not** auto-mount `/dev/sda1` or
> `/dev/sda3`. `ls /mnt/onl/boot` is **empty** on the live system — that's normal.
> Mount them yourself: `mkdir -p /mnt/b1 && mount /dev/sda1 /mnt/b1`.

A typical `/dev/sda1` after our 6.1 install:

```
arm-accton-as4610-54-r0.itb        <- slot A (main)   = 6.1 FIT
arm-accton-as4610-54-r0-test.itb   <- slot B (test)   = 6.1 FIT
boot-config                        <- SWI=images:EdgeNOS-4610-61-ownbuild.swi
main-510-backup.itb                <- previous 5.10 kernel (rollback)
main-419-backup.itb                <- 4.19 kernel (rollback)
```

---

## 3. `boot_slot`: setting it reliably

There are two ways to write the u-boot environment, and **only one is reliable here**:

- ✅ **From the OS / ONIE: `fw_setenv boot_slot B`** — persists. Use this.
- ❌ **From inside `bootcmd` (`saveenv`)** — on this u-boot it is **unreliable**; the
  self-revert line in `nos_bootcmd` often does *not* stick. Don't depend on it.

Read it back to be sure: `fw_printenv boot_slot`.

Because the in-`bootcmd` self-revert is flaky, **don't rely on "boot B once, auto-fall-
back to A."** If you want a one-shot test, set `boot_slot` explicitly before and after.

---

## 4. Install an image on BOTH slots (the proven procedure)

This is exactly what we did to make 6.1 the default on both slots. Everything runs on
the **running EdgeNOS** over serial (no ONIE). Host `10.1.1.30` serves the artifacts
over HTTP (see [`BUILD_AND_DEPLOY.md`](BUILD_AND_DEPLOY.md) §serial-toolkit).

```sh
# on the box (serial). bring up mgmt so it can fetch:
dhclient ma1

# --- the root filesystem image goes on the images partition (sda3) ---
mkdir -p /mnt/img && mount /dev/sda3 /mnt/img
wget -q http://10.1.1.30:8080/EdgeNOS-4610-61-ownbuild.swi \
     -O /mnt/img/EdgeNOS-4610-61-ownbuild.swi
md5sum /mnt/img/EdgeNOS-4610-61-ownbuild.swi          # MUST match the build host

# --- the kernel FIT + boot-config go on the boot partition (sda1) ---
mkdir -p /mnt/b1 && mount /dev/sda1 /mnt/b1
cp -n /mnt/b1/arm-accton-as4610-54-r0.itb /mnt/b1/main-<oldver>-backup.itb   # keep a rollback!
wget -q http://10.1.1.30:8080/arm-accton-as4610-54-r0-61.itb -O /tmp/new.itb
md5sum /tmp/new.itb                                   # verify before installing
cp /tmp/new.itb /mnt/b1/arm-accton-as4610-54-r0.itb        # slot A (main)
cp /tmp/new.itb /mnt/b1/arm-accton-as4610-54-r0-test.itb   # slot B (test)
printf 'NETDEV=ma1\nBOOTMODE=SWI\nSWI=images:EdgeNOS-4610-61-ownbuild.swi\n' \
     > /mnt/b1/boot-config
sync
```

That's it: both slots now point at the same 6.1 kernel + 6.1 SWI.

> **Autostart requires the matching modules.** The SWI has the BDE/KNET modules
> (`ko61`) baked into `/opt/edgenos`. They must match the kernel's vermagic or
> `bcmd.service` fails to load them. The own-build SWI is built to match — that's why
> `build-ownbuild-swi-61.sh` exists. If you boot a 6.1 kernel against a 5.10 SWI, the
> baked modules won't load (vermagic mismatch) and the datapath won't autostart.

---

## 5. Verify EACH slot boots the image

Test them one at a time so a bad slot can't hide behind a good one.

```sh
# --- Slot B ---
fw_setenv boot_slot B ; fw_printenv boot_slot ; sync ; reboot
#   on serial, confirm: u-boot "Loading file arm-accton-as4610-54-r0-test.itb"
#                       "Linux version 6.1.175...", login prompt, no Oops
#   then check the datapath autostarted:
systemctl is-active bcmd ; ping -c3 10.14.1.254 ; ping -c3 10.101.102.1

# --- Slot A ---
fw_setenv boot_slot A ; fw_printenv boot_slot ; sync ; reboot
#   confirm u-boot "Loading file arm-accton-as4610-54-r0.itb" (no -test), same checks
```

A fully-passing slot: boots `6.1.175`, baked modules load (`_get_cmic_ver: type
20000180`, no Oops), `bcmd` active, and **both ports ping** (copper `ge25` →
`10.14.1.254`, fiber `xe0` → `10.101.102.1`) with **zero manual steps**.

---

## 6. ⚠️ The reboot trap (and recovery)

**Never `reboot` from a *degraded* state** — i.e. if a kernel module has already
oopsed in this boot. The shutdown wedges ("`watchdog: watchdog0: watchdog did not
stop!`" + "`Could not detach loopback /dev/loop0: Device or resource busy`") and the
box hangs with the kernel dead — SysRq won't help. You'll need a **physical
power-cycle**. A clean cold boot is fine; it's only rebooting *out of* a broken state
that snags.

`panic=10` is in the bootargs, so a true kernel **panic** auto-reboots after 10s. A
*hang* (RCU stall, the shutdown wedge) does not — that's the power-cycle case.

### Recovering a box that won't boot
1. Catch u-boot on serial during cold boot (USB-serial **re-enumerates** on
   power-cycle, so retry-open both `/dev/ttyUSB0` and `/dev/ttyUSB1` while flooding
   `\r`). Tools: `live-investigation/tools/catch_only_uboot.py`,
   `reboot_catch_uboot.py`.
2. At the u-boot prompt: `setenv boot_slot A` (or whichever slot is known-good) and
   boot, **or** `run onie_rescue` to get into ONIE.
3. Worst case, point a slot's itb back at a backup: from ONIE/OS,
   `cp main-<ver>-backup.itb arm-accton-as4610-54-r0.itb`.

This is why we **always keep a backup itb** (`main-510-backup.itb`,
`main-419-backup.itb`) and why both slots having a known-good image is itself a safety
net.

---

## 7. Why one boot-config for two slots?

The ONL scheme has a single `boot-config`, so both slots share the **same rootfs SWI**.
The A/B mechanism only swaps the **kernel FIT**. Practically:

- For an A/B *kernel* test (e.g. old kernel on A, new kernel on B, same userland),
  put different FITs on the two itb names and flip `boot_slot`.
- For a full A/B with different *rootfs* too, you'd also swap `boot-config` (we keep
  `boot-config.<variant>` backups on sda1 and `cp` the one we want). There is no
  per-slot boot-config in this scheme.
- For "both slots are the same production image" (our 6.1 default), both itb names =
  the same FIT and the one boot-config = the matching SWI. Any reboot of either slot
  comes up identical.
