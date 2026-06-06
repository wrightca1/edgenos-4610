# edged boot integration — ONL persistence model & how to auto-start

## Why this is non-trivial on ONL

ONL **rebuilds `/` from the SWI on every boot**. Root is an overlay whose upper
layer is a RAM tmpfs (verified: `/` ≈ 113 MB, no backing partition in
`/proc/mounts`). So **edits to `/` (e.g. `/etc/init.d`, `/etc/ssh/sshd_config`,
`/etc/rc.local`, `/tmp`) do NOT survive a reboot.** `rc.local` in the running
system is the SWI's no-op stub, and nothing in ONL's boot path runs a script
from persistent storage.

Only these partitions persist (from `/proc/mounts` on the AS4610-54T):

| Mount | Device | RW | Purpose |
|---|---|---|---|
| `/mnt/onl/boot`   | sda1 | ro | loader + `arm-...-r0.itb` (kernel/initrd/dtb) |
| `/mnt/onl/config` | sda2 | ro | system config, persists across **installs/upgrades** |
| `/mnt/onl/images` | sda3 | ro | the installed SWI |
| `/mnt/onl/data`   | sda4 | **rw** | user/persistent data (28 GB) |

## Two ways to run edged at boot

### A. Bake into the SWI (the proper "our NOS" answer — survives installs)
Add to the ONL rootfs so every install auto-starts edged:
1. `edged` → rootfs `/usr/bin/edged`
2. `edged.init` → rootfs `/etc/init.d/edged` (chmod +x)
3. rc symlinks `/etc/rc[2345].d/S99edged -> ../init.d/edged`
4. (optional) persistent `sshd_config` `PermitRootLogin yes`

Reproducible path: contribute these via the ONL build (rootfs is assembled from
`$ONL/builds/any/rootfs/APKG.yml`), then `nos/build.sh installer` → new SWI/
installer. Requires a rebuild **and a re-flash** (`onie-nos-install`), which wipes
the current install — so it's gated on explicit user go.

Surgical path (no full rebuild): `unsquashfs` the SWI's `rootfs-armhf.sqsh`, drop
the 3 files in, `mksquashfs`, re-zip the SWI, regenerate the installer.

### B. Persistent partition + manual launch (works NOW, no re-flash)
`edged` is installed to `/mnt/onl/data/edge/edged` (persists across reboot as a
binary). Because there's no boot hook, start it by hand after boot:

```sh
ssh root@<mgmt-ip>            # root/onl (PermitRootLogin must be re-enabled — ephemeral)
/mnt/onl/data/edge/edged      # brings the BCM56340 data plane up
```

This is the stopgap until the SWI is rebuilt with option A.

## Status (2026-06-05)
- `edged.init` written (this dir). edged installed to `/mnt/onl/data/edge/` on the
  live box (option B). Option A (SWI bake + re-flash) staged, pending user go.
