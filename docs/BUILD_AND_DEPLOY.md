# EdgeNOS-4610 build & deploy pipeline (overview)

How the pieces fit together to go from kernel source → a running switch, plus the
serial/recovery toolkit and the hardware-verification routine. Start here, then dive
into the focused guides:

- [`ONIE_IMAGE_GUIDE.md`](ONIE_IMAGE_GUIDE.md) — build/install the from-scratch ONIE image
- [`AB_DUAL_SLOT_GUIDE.md`](AB_DUAL_SLOT_GUIDE.md) — the A/B slot scheme + in-place updates
- [`KERNEL_510_OWNBUILD.md`](KERNEL_510_OWNBUILD.md), [`KERNEL_419_PORT.md`](KERNEL_419_PORT.md) — per-kernel forward-port detail
- `../nos/kernel/README.md` — the kernel ladder status (4.14 → 4.19 → 5.10 → 5.15 → 6.1)

---

## 1. The board in one paragraph

Edgecore **AS4610-54T**: BCM56340 "Helix4" switch SoC with the **CPU on the switch
die** (dual Cortex-A9, armv7 hard-float), on-die CMICd, u-boot, ONIE. Because the CPU
is on the switch die, the kernel needs the out-of-tree **XGS-iProc BSP** (our
`brcm-iproc-<ver>.patch`) — mainline only has *generic* iProc, not this switch-SoC
family. The datapath is a userspace daemon, **`bcmd`** (BCM SDK app), talking to the
chip through the GPL **BDE/KNET** kernel modules.

---

## 2. The build pipeline

Four artifacts, four scripts. They layer bottom-up:

```
 (1) KERNEL FIT        nos/build-<ver>-fit.sh
       kernel Image (from the patched tree) + RTC-disabled board DTB + ONL initrd
       -> output/kport<ver>/arm-accton-as4610-54-r0-<ver>.itb
                              │
 (2) BDE/KNET MODULES  nos/datapath/build-bde-<ver>.sh  (+ inline knet build)
       SDK 6.5.16 GPL modules cross-built against the SAME kernel tree
       -> output/kport<ver>/ko<ver>/{linux-kernel-bde,linux-user-bde,linux-bcm-knet}.ko
       (vermagic MUST match the kernel; bcmd itself is kernel-independent)
                              │
 (3) OWN-BUILD SWI     nos/build-ownbuild-swi-<ver>.sh
       Buildroot rootfs (glibc+systemd, our overlay) with ko<ver> + datapath baked in
       -> output/kport<ver>/EdgeNOS-4610-<ver>-ownbuild.swi
                              │
 (4) ONIE INSTALLER    nos/build-<ver>-installer.sh
       wraps FIT + SWI + boot-config into a self-extracting .shar
       -> output/onie-installer-edgenos-<ver>-ownbuild
```

**The golden rule:** the BDE/KNET modules (step 2) are tied to the kernel by
*vermagic*. Rebuild the kernel → rebuild the modules → rebake the SWI. The own-build
SWI script bakes the matching `ko<ver>` so steps 2–3 stay in lockstep. (For 5.10 the
one-command `build-ownbuild-swi.sh` even rebuilds the modules first; for 6.1 the
modules were prebuilt + HW-validated so `build-ownbuild-swi-61.sh` just bakes them.)

### The kernel tree itself
A kernel version `<ver>` is reproduced as: pristine kernel.org tarball + our
`nos/kernel/patches/brcm-iproc-<ver>.patch` + the config (5.10+ also needs
`nos/kernel/config/cgroups-systemd.fragment` for systemd). The patch is the
forward-ported XGS-iProc BSP. See the per-kernel docs for the exact deltas.

> **Patch hygiene lesson:** generate canonical patches with
> `diff -uprN -X dontdiff` **and verify by applying to a *fresh pristine tree* AND
> building** — not just by reject count. `diff -rN` silently drops some new files
> under `include/` during full-tree recursion, which once left a patch that
> reject-applied cleanly but didn't build. Append such headers explicitly.

### Build environment
Everything compiles in the container **`edgenos/builder9:1.8-rootless`** (rootless
docker; `DOCKER_HOST=unix:///run/user/$(id -u)/docker.sock`), `ARCH=arm
CROSS_COMPILE=arm-linux-gnueabihf-`. Buildroot itself runs on the **host as non-root**
(Buildroot refuses to run as root). Build outputs live under `output/` and the
Buildroot tree under `ownbuild/build/` — both are **gitignored** (multi-GB); only
source (patches, scripts, overlay, configs) is tracked.

---

## 3. Deploying to the box

Two paths — pick by situation:

| situation | use | doc |
|-----------|-----|-----|
| fresh machine / reformat / recovery | ONIE installer (`onie-nos-install`) | [`ONIE_IMAGE_GUIDE.md`](ONIE_IMAGE_GUIDE.md) |
| box already runs EdgeNOS, swap kernel/rootfs | in-place A/B slot update (no ONIE) | [`AB_DUAL_SLOT_GUIDE.md`](AB_DUAL_SLOT_GUIDE.md) |

Both ultimately place the same three things (FIT on `sda1`, SWI on `sda3`,
`boot-config` on `sda1`) — the ONIE path just also reformats and runs from ONIE.

---

## 4. Serial + staging toolkit (how we drive the box headless)

The dev host is **`10.1.1.30`**; the box mgmt port is **`ma1`**. Serial console is
`/dev/ttyUSB1` @ 115200 (root / `onl`). Helpers live in `live-investigation/tools/`:

- `sercmd.py "<cmd>" [wait]` — send one command, print the reply.
- `catch_only_uboot.py` / `reboot_catch_uboot.py` — catch the u-boot prompt on cold
  boot (re-enum tolerant — see below).

**Staging files to the box (HTTP):** run a tiny server on the host and `wget` from the
box. Note: the harness blocks `python3 -m http.server` (it exits 144) — use a small
`socketserver` script instead (binding a listening socket works fine that way):

```python
import http.server, socketserver, os
os.chdir('/.../output/kport61')
socketserver.TCPServer.allow_reuse_address = True
with socketserver.TCPServer(('10.1.1.30', 8080), http.server.SimpleHTTPRequestHandler) as h:
    h.serve_forever()
```

### Serial gotchas that wasted our time
- **USB-serial re-enumerates on power-cycle/reset** — the host `/dev/ttyUSB1` fd goes
  stale and may reappear as `ttyUSB0`. A robust catcher **retry-opens both nodes** in
  a loop. A single `open()` will miss the boot.
- **`ma1` does not auto-DHCP.** After every boot, run `dhclient ma1` before the box can
  `wget` from the host. (`ma1` is the bgmac mgmt NIC — its driver `BGMAC_PLATFORM`
  needs `ARCH_XGS_IPROC` in the BSP patch, which is why that Kconfig hunk is
  load-bearing, not cosmetic.)
- **`wget -O file` leaves a 0-byte file on failure** (e.g. `ma1` down). It then
  *clobbers* whatever was there. **Always re-verify md5 after a download**, especially
  when overwriting a slot or SWI.
- **A login prompt sitting idle emits no bytes** — "no serial output" can mean "already
  booted, waiting at login," not "hung." Send a `\n` and check for an echo before
  concluding it's wedged.

---

## 5. Hardware verification routine

What "it works" means on this box, and how we check it after any deploy:

1. **Kernel up:** `uname -r` shows the expected version; boot reached `login:` with no
   `Unable to handle kernel` / `Oops` / `panic` on the serial log.
2. **Modules loaded:** `lsmod | grep -E 'bde|knet'` shows all three; dmesg shows
   `_get_cmic_ver: ... type 20000180` (the on-die CMIC was detected and BDE init
   completed). A *vermagic mismatch* line means the SWI's baked modules don't match the
   kernel — rebuild/rebake.
3. **Datapath daemon:** `systemctl is-active bcmd` = `active`.
4. **Ports forward (the real test):**
   ```sh
   ping -c3 10.14.1.254     # copper  ge25  (10.14.1.2/29 side)
   ping -c3 10.101.102.1    # fiber   xe0   (SFP+, 10.101.102.2/29 side)
   ```
   Both at 0% loss = the full stack (kernel + BDE/KNET + CMIC DMA + bcmd + L3 +
   chip forwarding) is healthy. With the own-build SWI this happens **unattended** on
   boot (`bcmd.service` autostart); for a manual/first bring-up you load the modules,
   `mknod` the devnodes, deassert the CPLD PHY resets, then start `bcmd` (see
   `ownbuild/config/overlay/opt/edgenos/bcmd-prep.sh`).

> If a port is down: `cat /sys/class/net/<if>/carrier` (1 = our side up) and
> `ip neigh show dev <if>` (resolved vs FAILED) separate a local link problem from a
> peer/cable problem. The SFP+ fiber in particular has been peer-gated before (far-end
> router rebooting) — that's not a switch fault.

---

## 6. Where to make changes

| want to change… | edit… | then run… |
|-----------------|-------|-----------|
| kernel config | the saved `.config` / `cgroups-systemd.fragment` | rebuild kernel + `build-<ver>-fit.sh` |
| BSP source | regenerate `brcm-iproc-<ver>.patch` (verify from pristine!) | rebuild kernel |
| SDK/BDE/KNET source | `sdk-6.5.16-linux<ver>-compat.patch` (in `nos/datapath/`) | `build-bde-<ver>.sh` |
| rootfs contents / services | `ownbuild/config/overlay/...` | full buildroot rebuild, or `build-ownbuild-swi-<ver>.sh` (re-syncs overlay systemd units) |
| datapath bring-up steps | `ownbuild/config/overlay/opt/edgenos/bcmd-prep.sh` | rebake SWI |
| L3 addresses / routes | `ownbuild/config/overlay/etc/edged/*.conf` | rebake SWI |
