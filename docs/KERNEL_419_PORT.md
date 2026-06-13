# AS4610-54T — Linux 4.14 → 4.19.81 forward-port (Helix4 / brcm-iproc)

Status (2026-06-12): **4.19.81 boots clean on real hardware, datapath works, autostart
wedge SOLVED.** This is Step 1 of the kernel ladder (see `nos/kernel/README.md`). Step 2 =
4.19 → 5.10.

Kernel release string: `4.19.81-OpenNetworkLinux-armhf`. Board: Edgecore AS4610-54T,
BCM56340 "Helix4" SoC, dual Cortex-A9, on-die CMICd, ARM/u-boot.

---

## 1. Artifacts (in `output/kport419/`)

| file | md5 | what |
|------|-----|------|
| `linux-4.19.81/` | — | working kernel tree (pristine 4.19.81 + the patch below, built) |
| `arm-accton-as4610-54-r0-419f.itb` | `c146e335` | **canonical kernel FIT** = clock-fix kernel + RTC-disabled DTB (byte-reproducible from source). `b886d194` was the original hand-built FIT — content-identical (only the mkimage timestamp field differs) and is the one currently running on the box. |
| `arm-accton-rtcdis.dtb` | `64a1c2e2` | RTC-disabled board DTB (`dtc` of the flat DTS below) |
| `arm-accton-as4610-54-r0-419e.itb` | `7dbf0003` | clock-fix kernel + stock DTB (RTC still wedges — superseded) |
| `EdgeNOS-4610-419-auto.swi` | `36ded2e5` | autostart rootfs SWI (datapath + rc.local), see §6/§8 |
| `edgenos-419f.its` | — | FIT source (kernel + rtcdis DTB + initrd) for 419f |

**Productionized build scripts (all reproducible, run in `edgenos/builder9:1.8-rootless`):**
- `nos/kernel/patches/brcm-iproc-4.19.patch` (md5 `602c3698`, 41067 lines, 163 files) — applies
  `-p1` to pristine `linux-4.19.81`, 0 rejects.
- `nos/kernel/dts/arm-accton-as4610-rtcdis.dts` — checked-in **flattened** RTC-disabled DTS (the
  "nice" `arm-accton-as4610.dts` `#include`s `bcm-helix4.dtsi` which isn't vendored standalone;
  this flat DTS is decompiled from the working ONL platform DTB + `rtc@68 disabled` and `dtc`'s to
  the exact verified DTB `64a1c2e2`).
- `nos/build-419f-fit.sh` — `dtc` DTS→DTB + `gzip` Image + `mkimage` → 419f FIT. Byte-reproducible
  (pins `SOURCE_DATE_EPOCH`); emits `c146e335`.
- `nos/build-419f-installer.sh` — reuses ONL's `installer.sh`/sfx, swaps in the 419f FIT + auto SWI
  + explicit `boot-config` (`SWI=images:…auto.swi`), recomputes the loader-initrd offset/size via
  `pyfit`, repackages with `mkshar` → **`output/onie-installer-edgenos-419f`** (md5 `948facb2`).
  This is the from-scratch / reflash installer (ONIE: `onie-nos-install`). NOT yet install-tested
  end-to-end (the box is on the equivalent manual deploy).

Pristine source: `OpenNetworkLinux/packages/base/any/kernels/archives/linux-4.19.81.tar.xz`.
Reference 4.14 patch/installer: `OpenNetworkLinux/.../4.14-lts/patches/brcm-iproc-4.14.patch`,
`tools/mkinstaller.py` + `tools/mkshar` + `tools/scripts/sfx.sh.in`.

---

## 2. How the patch was produced / how to reproduce the tree

```sh
tar xf .../archives/linux-4.19.81.tar.xz                 # pristine
cd linux-4.19.81
patch -p1 < nos/kernel/patches/brcm-iproc-4.19.patch     # forward-port + all fixes
# config: start from 4.14 armhf-iproc-all.config -> make olddefconfig (keeps the
#   XGS_IPROC family + CONFIG_IP_ROUTE_MULTIPATH + CONFIG_IPV6_MULTIPLE_TABLES)
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j"$(nproc)" Image modules
```
Toolchain lives in the build container `edgenos/builder9:1.8-rootless`
(`arm-linux-gnueabihf-gcc-8`). Run via `docker run -u root:0 -v $EDGE:$EDGE -w <kerndir>`.

The patch = (the original 32k-line `brcm-iproc-4.14.patch` forward-ported to 4.19) + the
fixes in §4. Files that were *reverted to pristine 4.19 mainline* (sp805_wdt.c, spi-bcm-qspi.c)
correctly produce NO diff entry — they match pristine.

---

## 3. Forward-port: rejects from applying the 4.14 patch to pristine 4.19

The 32,416-line `brcm-iproc-4.14.patch` applied with only ~10 real rejects of 32 failed hunks
(rest auto-merged with fuzz). New out-of-tree drivers (CMIC core under
`drivers/soc/bcm/xgs_iproc/`, `arch/arm/mach-iproc/`, serdes phy, `xgs_iproc_smbus`,
`xgs-iproc-flash`, `phy-xgs-iproc`, `gpio-xgs-iproc`, …) landed clean (new files).

Resolved during port: `arch/arm/boot/dts/Makefile` (iproc dtb block placement — must go AFTER
the full ARCH_ASPEED list), `drivers/char/hw_random/Makefile`, `drivers/mmc/host/Makefile`,
`drivers/net/ethernet/broadcom/Kconfig`, `drivers/usb/host/{ehci,ohci}-platform.c`.

Deferred / dropped (not needed for armhf Helix4): the 5 `tools/power/acpi/*` hunks, the
arm64 helix5 dts, and `drivers/net/phy/phy.c` (the iProc shared-MDIO re-schedule workaround —
4.19 rewrote the PHY state machine; it's an MDIO-contention optimization, not needed for
bring-up; the `.rej` is kept in the tree).

---

## 4. The four compile/boot fixes folded into the 4.19 patch

1. **`arch/arm/mach-iproc/board_bu.c`** — `init_dma_coherent_pool_size()` was removed in 4.19.
   Dropped the call; size the atomic DMA pool via the `coherent_pool=16M` boot arg instead
   (already in `nos_bootcmd`).
2. **`drivers/spi/spi-bcm-qspi.c`** — REVERTED to pristine 4.19 (4.19 converted BSPI flash read
   to the spi_mem framework; the 4.14 hunk was obsolete and half-applied).
3. **`drivers/watchdog/sp805_wdt.c`** — REVERTED to pristine 4.19. The 4.14 hunk applied HALF
   (declared/used `wdt_bootstatus` but the init `of_iomap` hunk rejected → NULL deref →
   `sp805_wdt_probe` crash on every boot, PID 1). The mainline sp805 watchdog works without the
   iProc bootstatus reg read.
4. **`drivers/usb/host/{ehci,ohci}-platform.c`** — moved `struct usb_phy *phy` decl to function
   scope (4.19 dropped phy_num/rst from the probe decls).

Plus the **iProc i2c clock-frequency fix** in `drivers/i2c/busses/xgs_iproc_smbus.c` (see §5).

---

## 5. Root causes of the two i2c problems (the debugging story)

### 5a. i2c0 / CPLD clock-frequency enum regression (real, but BENIGN — not the wedge)
`iproc_smb_set_clk_freq()` takes an *enum* (`I2C_SPEED_100KHz=0`, `I2C_SPEED_400KHz=1`); any
other value hits `default: return -EINVAL` and leaves the clock divider unprogrammed (caller
ignores the rc). The AS4610 DTB sets `&i2c0 { clock-frequency = <400000>; }` (the CPLD bus).
The 4.19 port dropped Edgecore's Hz→enum conversion, so it passed raw `400000` as the enum →
`-EINVAL`. **Fix:** restore the conversion (400000 → `I2C_SPEED_400KHz`, else 100KHz) in
`iproc_smb_init()`.
NOTE: this did NOT cause the boot wedge. Even with the clock correctly programmed at 400KHz the
135 "Error in transaction" messages on `18038000.i2c` persist at boot — they're benign
probe-time NAKs that 4.14 silently swallowed (the 4.19 forward-port un-`#ifdef`'d the
`IPROC_SMB_DBG` `dev_err`/`dev_info` prints). Cosmetic log noise.

### 5b. i2c1 / RTC livelock (THE autostart wedge)
SysRq-w on the wedged box (pings but no sshd/getty):
```
kworker D  Workqueue: events rtc_timer_do_work
  rtc_timer_do_work -> __rtc_set_alarm -> __rtc_read_time -> ds1307_get_time
    -> regmap -> i2c_smbus_read_byte_data -> __i2c_mux_smbus_xfer -> iproc_smb_xfer -> msleep
```
The ds1307/**M41T11** RTC has a dead battery (reads 2001). The RTC core perpetually re-arms an
already-expired alarm; each pass is a **muxed** i2c read (mux-select → read → deselect on the
pca9548 behind `&i2c1`) that holds the i2c1 `down(&xfer_lock)` semaphore. `onlpd`'s sensor
reads block on that lock → ONL boot stalls before sshd/getty. The kernel stays alive (ICMP +
SysRq work) — it's a bus-lock livelock, not a panic.

**Why 4.14 never wedged:** the ds1307 driver and the RTC core are essentially unchanged
4.14→4.19. The difference is in `xgs_iproc_smbus.c`: Edgecore's 4.14 driver had the trailing
per-transaction `msleep(1)` (held inside `xfer_lock`) **commented out** to speed up EEPROM
dumps; the 4.19 forward-port **re-added it**. +1ms × every muxed transaction × the re-arm burst
holds the bus lock long enough at boot to starve onlpd. On 4.14 the same RTC activity flew by
fast enough that onlpd always got the bus between reads.

**Fix:** disable the unused RTC in the DTB — `rtc@68 { status = "disabled"; }`. Driver never
probes → no `rtc0` → `rtc_timer_do_work` never scheduled → i2c1 lock never held → onlpd
completes → boot finishes. NTP sets the clock. (Alternative/additional fix not taken: re-remove
the `msleep(1)` to match the 4.14 baseline — left in place so the patch reproduces the
confirmed-working 419f exactly.)

**Confirmed on HW (419f):** boots to `localhost login:` in 81s, `rtc-ds1307` probes 0×, 0
D-state tasks, loadavg settling. Wedge gone.

---

## 6. FIT assembly + deploy

FIT = kernel (`kernel-4.19-iproc.bin.gz`, load/entry `0x61008000`) + DTB
(`arm-accton-as4610-rtcdis.dtb`) + `onl-loader-initrd-armhf.cpio.gz`, built with `mkimage -f
edgenos-419f.its <out>.itb`. The loader FIT lives on `/dev/sda1` (`/mnt/onl/boot`) as
`arm-accton-as4610-54-r0.itb`; u-boot `nos_bootcmd` does `ext2load usb 0:1 0x70000000 <itb>;
bootm 0x70000000#arm-accton-as4610-54-r0`. BDE/KNET `.ko` are vermagic-tied to the release
string (unchanged by these fixes) so they need NO rebuild.

**Deploy a new kernel itb** (box already installed): boot ONIE rescue (§7) and over SSH:
```sh
mount /dev/sda1 /mnt/b
wget http://<host>:8080/<new>.itb -O /mnt/b/arm-accton-as4610-54-r0.itb   # verify md5
sync; /sbin/reboot
```

### Current on-box slot layout (`/mnt/onl/boot/`)
- `arm-accton-as4610-54-r0.itb` = **419f** (`b886d194`) — default boot
- `slotA-419d.itb` = 419d (`f92f1014`) — emergency fallback (boots, but RTC-wedges)
- `arm-accton-as4610-54-r0-test.itb` = 419f — one-shot test slot (slot B)
- u-boot env `boot_slot=A` (boots the main itb)

---

## 7. Recovery toolkit (proven this session)

The box console login is **root / onl** (serial). SSH `PasswordAuthentication` is OFF on the
OS, so use serial for the OS; ONIE has its own SSH.

- **USB-serial re-enumerates on every reset** — the host `/dev/ttyUSB1` fd goes stale, so a
  single open() misses u-boot. Use a **retry-open loop** while flooding `\r`:
  `live-investigation/tools/reboot_catch_uboot.py` (SysRq-b + re-enum-tolerant catch).
- **Reboot a kernel-alive-but-userspace-wedged box:** SysRq over serial — `send_break` then `b`
  (the catch script does this). SysRq-w dumps blocked tasks; SysRq-s syncs.
- **u-boot → ONIE rescue:** at the `accton_as4610-54->` prompt, `run onie_rescue`. ONIE DHCPs
  (got the same `10.1.1.209`) and starts dropbear. SSH in with **legacy host-key algos**:
  `ssh -o HostKeyAlgorithms=+ssh-rsa,ssh-dss -o PubkeyAcceptedKeyTypes=+ssh-rsa root@<ip>`
  (root, empty password). ONIE has `wget`/`mount`/`md5sum`/`/sbin/reboot` (not in PATH → full
  path). `onie_boot_reason` is NOT set by `run onie_rescue`, so a plain reboot returns to the OS.
- **Serial helpers:** `tools/sercmd.py` (one cmd), `tools/serlogin.py` (expect-style login+cmd).
  Serial echo is mangled in DISPLAY but the bytes received are clean — always **verify via
  read-back** (e.g. `fw_printenv`, `md5sum`), never trust the echo.

### A/B one-shot slot failsafe — and its caveat
`nos_bootcmd` was extended so `boot_slot=B` boots `…-test.itb`. INTENDED behaviour: the B branch
does `setenv boot_slot A; saveenv` *before* `bootm` so a hang auto-reverts on next boot.
**CAVEAT: this u-boot's `saveenv` from inside `bootcmd` does NOT persist** — `boot_slot` stayed
`B`. So the auto-revert is unreliable here; manage `boot_slot` via `fw_setenv` from the OS/ONIE
(that path DOES write the env). The real, deterministic recovery levers are the on-disk fallback
itb + uboot-catch + ONIE-SSH above. Original `nos_bootcmd` is stashed in env `nos_bootcmd_orig`.

---

## 8. Autostart on 419f — ✅ CONFIRMED (2026-06-12)

`EdgeNOS-4610-419-auto.swi` (md5 `36ded2e5`, rootfs-only: stock ONL rootfs + our datapath
overlay + `rc.local` autostart) deployed to `/mnt/onl/images/` and selected explicitly via
`boot-config` (`SWI=images:EdgeNOS-4610-419-auto.swi` — NOT `::latest`, which is ambiguous since
our SWI reuses the stock manifest timestamp). Booted on the 419f kernel:
- Clean boot to login at 84s, NO wedge (RTC fix held).
- `rc.local` → 30s settle → `/etc/init.d/edgenos start` fired; boot log:
  `platform prep + bcmd → waiting for datapath UP → L3 (ge25 10.14.1.2/24, xe0 10.101.102.2/29)
  → Quagga → up bcmd=1724 zebra=2081 ospfd=2085`.
- bcmd running, all 3 BDE/KNET modules loaded, xe0+ge25 UP/LOWER_UP.
- **Forwarding verified: ping 10.101.102.1 (xe0 SFP+) = 0% loss; ping 10.14.1.254 (ge25 copper)
  = 0% loss.** Fully unattended from cold boot.

The box's durable state is now: 419f kernel (sda1) + `EdgeNOS-4610-419-auto.swi` (boot-config) →
a power-cycle reproduces a working autostarting datapath. (Known unrelated nit: ma1 mgmt needs
`dhclient ma1` — it doesn't auto-DHCP; the datapath front ports are unaffected.)
`boot-config.stock` backed up alongside on sda1.

## 9. Productionized (2026-06-12)

- ✅ Reproducible FIT-from-source (`nos/build-419f-fit.sh`) — DTB compiled from the checked-in flat
  DTS, byte-stable FIT `c146e335`.
- ✅ Reflashable onie-installer (`nos/build-419f-installer.sh` → `output/onie-installer-edgenos-419f`,
  md5 `948facb2`): bundles the 419f FIT + autostart SWI + explicit `boot-config`. A fresh/reflashed
  AS4610 installed via `onie-nos-install` boots straight into the autostarting datapath.

## 10. onie-installer — ✅ INSTALL-TESTED FROM SCRATCH (2026-06-12)

`onie-nos-install http://<host>:8080/onie-installer-edgenos-419f` from ONIE rescue did a real
clean install: boot partition reformatted to just `arm-accton-as4610-54-r0.itb` (FIT `c146e335` =
installer payload) + explicit `boot-config`; auto SWI in images. Cold-booted into the installed
NOS and **autostarted the datapath: bcmd + L3 + Quagga, ping 10.101.102.1 (xe0) 0% + 10.14.1.254
(ge25) 0%.** From-scratch install → working autostarting datapath, fully validated.
GOTCHA: `onie-nos-install` calls `reboot`; ONIE's SSH login has a minimal PATH without `/sbin`, so
the first run failed `reboot: not found` (and did NOT install). Run it as
`export PATH=$PATH:/sbin:/usr/sbin; onie-nos-install <url>` (ONIE's own auto-discovery path is fine).

## 11. Remaining work

1. Step 2: **4.19 → 5.10** (matches the 5610; switch BDE/KNET to SDK 6.5.27).
