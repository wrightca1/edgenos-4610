# Building an ONIE-installable image for the AS4610 (the practical guide)

ONIE's own docs describe a *framework*; they don't tell you what an installer file
actually **is** or how to build one by hand. This is the version we wish we'd had.

---

## TL;DR

An "ONIE NOS installer" is **just a self-extracting shell script** with a zip stapled
to the end. ONIE downloads it, runs it as a shell script, and the script copies a
kernel + root filesystem onto the disk and sets up the bootloader. That's the whole
trick. For the AS4610 we build one with:

```sh
nos/build-61-installer.sh        # -> output/onie-installer-edgenos-61-ownbuild
```

Then on the switch, from the ONIE prompt:

```sh
export PATH=$PATH:/sbin:/usr/sbin           # <-- ONIE's shell forgets these; see gotchas
onie-nos-install http://10.1.1.30:8080/onie-installer-edgenos-61-ownbuild
```

The rest of this document explains every part so you can change it confidently.

---

## 1. What is actually inside an ONIE installer?

An ONIE installer is a **`.shar`** — a shell archive. Open it in a text editor and
you'll see a normal shell script for the first few KB, then binary garbage. The
shell part (the "sfx prefix") finds where the binary starts, unzips it to a temp
dir, and runs `installer.sh` from inside.

```
┌─────────────────────────────────────────────┐
│  #!/bin/sh   (sfx prefix from sfx.sh.in)     │  <- human-readable shell
│  ...finds the zip offset, unzips payload...  │
│  ...runs ./installer.sh...                   │
├─────────────────────────────────────────────┤
│  ZIP payload:                                │  <- binary
│    installer.sh         (the real installer) │
│    onl-loader-fit.itb   (kernel+dtb+initrd)  │
│    EdgeNOS-...-ownbuild.swi  (root fs image) │
│    boot-config          (which SWI to boot)  │
│    autoperms.sh preinstall.sh postinstall.sh │
│    config/  plugins/                         │
└─────────────────────────────────────────────┘
```

Key insight: **a bare `.swi` is NOT ONIE-installable.** ONIE needs the `.shar`
wrapper. The `.swi` is only the root filesystem; the installer also has to place the
kernel (the FIT) and the `boot-config`.

### The three payload pieces that matter

| piece | what it is | built by |
|-------|-----------|----------|
| **loader FIT** (`*.itb`) | u-boot FIT image = gzipped kernel `Image` + board DTB + ONL loader-initrd | `nos/build-61-fit.sh` |
| **SWI** (`*.swi`) | a zip of `rootfs-armhf.sqsh` (squashfs root) + `manifest.json` | `nos/build-ownbuild-swi-61.sh` |
| **boot-config** | 3 lines telling the loader which SWI to boot | written by the installer script |

`boot-config` looks like this — note it names an **explicit** SWI, never `::latest`
(which is ambiguous when you reuse a stock manifest timestamp):

```
NETDEV=ma1
BOOTMODE=SWI
SWI=images:EdgeNOS-4610-61-ownbuild.swi
```

---

## 2. How `build-61-installer.sh` works (and why each step exists)

We don't hand-write `installer.sh`/`sfx` — we **reuse ONL's** from a stock installer
payload and only swap in our FIT, SWI, and boot-config. The script
(`nos/build-61-installer.sh`, mirrors the 4.19/5.10 ones) does:

1. **Unzip a stock installer** to get a known-good `installer.sh`, `autoperms.sh`,
   `preinstall.sh`, `postinstall.sh`, `config/`, `plugins/`.
2. **Drop in our bits**: copy our FIT to `onl-loader-fit.itb`, copy our SWI, and
   write the explicit `boot-config`.
3. **Recompute the loader-initrd offset** (THE gotcha — see §3).
4. **Repackage** with ONL's `mkshar`:
   ```sh
   python2 OpenNetworkLinux/tools/mkshar --lazy --unzip-pad --fixup-perms autoperms.sh \
     out.shar OpenNetworkLinux/tools/scripts/sfx.sh.in installer.sh \
     onl-loader-fit.itb <swi> boot-config preinstall.sh postinstall.sh config plugins
   ```
5. The resulting `out.shar` is the installer. `chmod +x`, done.

Everything runs in the build container `edgenos/builder9:1.8-rootless` because
`mkshar` is a **python2** tool.

---

## 3. THE gotcha: the loader-initrd offset/size

This is what cost us the most time. `installer.sh` doesn't keep the loader-initrd as
a separate file — it **carves it out of the FIT at install time** with `dd`:

```sh
# inside installer.sh, paraphrased:
dd if=onl-loader-fit.itb bs=1 skip=$initrd_offset count=$initrd_size of=/mnt/.../initrd
```

Those two numbers, `initrd_offset` and `initrd_size`, are **specific to your FIT**.
If you swap in a new kernel FIT but leave the stock offsets, the installer carves out
garbage and the box won't boot. You must recompute them for *your* `.itb`:

```sh
# ONL ships a helper, pyfit, that reports the byte range of the initrd subimage:
PYTHONPATH=$VONL/src/python python2 $VONL/src/bin/pyfit offset <your.itb> --initrd
#   -> prints two numbers: FIRST_BYTE  LAST_BYTE   (inclusive)
# then:
initrd_offset = FIRST_BYTE
initrd_size   = LAST_BYTE - FIRST_BYTE + 1     # <-- +1 because both ends are inclusive
```

`build-61-installer.sh` does this automatically and `sed`s the two values into
`installer.sh` before repackaging. If you ever build an installer by hand, **do not
skip this**.

(`$VONL` = `OpenNetworkLinux/packages/base/all/vendor-config-onl`.)

---

## 4. Getting into ONIE and running the install

### Reach the ONIE prompt
From the u-boot console (serial), the cleanest way:

```
accton_as4610-54-> run onie_rescue      # boots ONIE in rescue mode (gets a shell + sshd)
```
(`run onie_install` is the discovery-install path; `onie_rescue` just drops you to a
shell so you can install manually.)

### SSH into ONIE (it uses ancient crypto)
ONIE's dropbear only offers legacy algorithms, so a modern client needs:

```sh
ssh -o HostKeyAlgorithms=+ssh-rsa,ssh-dss -o PubkeyAcceptedKeyTypes=+ssh-rsa root@<box-ip>
# user root, empty password
```

### Install
```sh
export PATH=$PATH:/sbin:/usr/sbin
onie-nos-install http://10.1.1.30:8080/onie-installer-edgenos-61-ownbuild
```

`onie-nos-install` downloads the installer, runs it, and reboots into the NOS.

---

## 5. Gotchas we actually hit (each one cost real time)

- **`reboot: not found`.** ONIE's SSH login session has a minimal `PATH` without
  `/sbin`. `onie-nos-install` calls `reboot` at the end, so without the PATH fix the
  install *appears* to run but never completes. Always
  `export PATH=$PATH:/sbin:/usr/sbin` first. (ONIE's own auto-discovery path is fine;
  this only bites interactive SSH.)
- **`fw_setenv` prompts `[N/y]`.** In ONIE, setting a u-boot var interactively waits
  for confirmation. In a script, pipe it: `printf 'y\n' | fw_setenv onie_boot_reason install`.
- **A bare `.swi` won't install.** You need the `.shar` from `mkshar`.
- **Don't symlink fakes** (`fdisk`/`mkfs`/`fw_setenv`/`reboot`) to "help" the
  installer — it works as-is once the PATH is right. We wasted time over-helping.
- **Stale initrd offsets** = unbootable install (see §3).
- **`unzip` warns / exits non-zero** about the SFX sh-prefix offset when you extract a
  `.shar` payload — that's expected; the files still extract. The build script
  tolerates it with `|| true`.

---

## 6. Reflash vs. in-place: which do you want?

- **`onie-nos-install` (this doc)** = *from scratch*. Reformats the disk, installs the
  kernel + SWI + boot-config fresh. Use for a clean machine, a recovery, or to change
  the partition layout.
- **In-place dual-slot update** = the box is already running EdgeNOS and you just want
  to swap the kernel/rootfs on one or both A/B slots **without ONIE**. That's the
  faster path and is documented separately in
  [`AB_DUAL_SLOT_GUIDE.md`](AB_DUAL_SLOT_GUIDE.md). You do **not** need ONIE to update a
  slot — only to reformat/reinstall.

---

## 7. File map

| file | purpose |
|------|---------|
| `nos/build-61-installer.sh` | build the 6.1 ONIE installer (mirror exists for 4.19, 5.10) |
| `nos/build-61-fit.sh` | build the loader FIT (kernel + DTB + initrd) |
| `nos/build-ownbuild-swi-61.sh` | build the SWI (squashfs root + manifest) |
| `OpenNetworkLinux/tools/mkshar` | ONL's shar packager (python2) |
| `OpenNetworkLinux/tools/scripts/sfx.sh.in` | the self-extracting shell prefix |
| `.../vendor-config-onl/src/bin/pyfit` | reports FIT subimage byte offsets |
| `output/onie-installer-edgenos-61-ownbuild` | the finished installer |
