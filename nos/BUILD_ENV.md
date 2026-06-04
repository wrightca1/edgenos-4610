# Build Environment — EdgeNOS-4610

What `build.sh` needs, and the status on this build host (2026-06-04).

## Host requirements (from ONL `docs/Building.md`)

- **Docker** — ONL builds inside a containerized workspace.
- **binfmt-support / qemu-user** — cross-arch (armhf) execution during build.
- **~40 GB free disk**, ≥4 GB RAM + swap.

## This host

| Item | Status |
|---|---|
| Docker | ✅ 24.0.2, **rootless** — socket at `/run/user/1000/docker.sock`. `build.sh` sets `DOCKER_HOST` automatically. |
| ONL tree | `../../OpenNetworkLinux` (git checkout). |
| ONL submodules | ✅ fetched: `sm/infra`, `sm/bigcode`, `sm/build-artifacts`. |
| Builder image | `dentproject/builder9:1.8` (stretch/armhf) — pulling. `builder10:1.2` also present (unused; buster). |
| Arch | x86_64 host, cross-building **armhf**. |

## Suite/arch note (important)

ONL builds **armhf under the `stretch` (Debian 9) suite** — see the top-level
`Makefile`: `BUILD_ARCHES_stretch := arm64 amd64 armel armhf`. So we use
`onlbuilder -9` + `dentproject/builder9:1.8`, **not** buster/builder10. The
AS4610-54 ONLP package is declared `ARCH=armhf TOOLCHAIN=arm-linux-gnueabihf`.

## One-time setup (already done here; recorded for reproducibility)

```bash
cd ../../OpenNetworkLinux
git submodule update --init --depth 1 sm/infra sm/bigcode sm/build-artifacts
export DOCKER_HOST=unix:///run/user/$(id -u)/docker.sock
docker pull dentproject/builder9:1.8
```

`platforms-closed` and the legacy-kernel / buildroot-mirror submodules are **not**
required for the as4610-54 armhf installer and are skipped (the first uses an
SSH/private remote that will fail anyway).

## Build

```bash
./build.sh packages    # quick: compile just the AS4610-54 platform packages
./build.sh             # full: armhf ONIE installer
./build.sh shell       # interactive builder shell (env pre-sourced)
```

Output installer:
`../../OpenNetworkLinux/RELEASE/stretch/armhf/ONL-*ARMHF*INSTALLER`

## Rootless-Docker fixes (required — learned the hard way)

The stock ONL builder assumes rootful Docker. Three problems and their fixes,
all baked into `build.sh` + the patched image:

1. **binfmt_misc mount aborts the build.** The in-image `/bin/docker_shell`
   does `sudo mount binfmt_misc` (and `/etc/init.d/binfmt-support start`), which
   fails under rootless (no host CAP_SYS_ADMIN; `/proc` paths can't be
   bind-mounted either). **Fix:** a derived image
   `edgenos/builder9:1.8-rootless` (see `scripts/builder-patch/`) with a patched
   `docker_shell` that makes those calls non-fatal. It's unnecessary anyway —
   the **host** registers qemu with the `F` (fix-binary) flag, which the kernel
   applies to exec globally regardless of the container's mount namespace.
   Build it once: `scripts/build-builder.sh`.

2. **Command quoting is destroyed.** `onlbuilder` and `docker_shell` both flatten
   everything after `-c` with a plain `" ".join` and re-run it through a shell,
   so a compound `source setup.env && make …` gets mangled (and `bash -l` tries
   to read an unreadable profile). **Fix:** `build.sh` writes the build commands
   to a script file inside the mounted ONL tree (`.edgenos-inner.sh`) and invokes
   that single path — no quoting to lose.

3. **uid mapping → "Permission denied" on mounted files.** Rootless maps host
   uid 1000 → *container root*, so bind-mounted files are owned by container-root,
   but `docker_shell` `sudo`s to a non-root user that can't read them. **Fix:**
   run as container root via `--user root:0`. Because of the same mapping, build
   outputs come back owned by **you** on the host (not root-owned).

## EOL-Debian (stretch) rootfs fix

The main rootfs is multistrap'd from Debian **stretch**, which is end-of-life.
ONL's generated apt sources route through **apt-cacher-ng** at `127.0.0.1:3142`
in front of `http://archive.debian.org/debian` (stretch lives in the archive).
Two things make this work:

- **apt-cacher-ng must be running.** ONL only auto-starts it in `--isolate`
  mode; `build.sh` starts it in the inner script (`/etc/init.d/apt-cacher-ng
  start`). Without it, the rootfs apt step fails with `E: Unable to locate
  package cpio` (apt can't reach the dead proxy).
- archive.debian.org serves the expired `Release` files fine for our purposes;
  the build already passes `Apt::Get::AllowUnauthenticated=true`.

Verified: the proxy returns the stretch armhf index (49,437 packages, incl.
`cpio`) end-to-end.

## Known gotchas

- ONL is in upstream maintenance/archival mode; some package downloads may 404.
  The `packages` target isolates platform-code compile from full-image fetches,
  so use it first to prove our target builds.
- Rootless docker maps your UID; build outputs under the ONL tree are owned by
  you (no root-owned artifacts), unlike the 5610 SDK build.
