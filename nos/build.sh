#!/usr/bin/env bash
#
# build.sh — build the ONL armhf ONIE installer for the Edgecore AS4610-54T.
#
# Drives the shared ONL tree at ../../OpenNetworkLinux inside the ONL "builder9"
# (Debian stretch) Docker workspace, which is the suite ONL uses for armhf.
#
# Phase 1 of EdgeNOS-4610: ONL-derived image on the proven iProc 4.14 kernel.
#
# Usage:
#   ./build.sh                 # build the full armhf installer
#   ./build.sh packages        # build just the as4610-54 platform packages (faster sanity build)
#   ./build.sh shell           # drop into an interactive builder shell
#
set -euo pipefail

# --- paths ---
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ONL="$(cd "$HERE/../../OpenNetworkLinux" && pwd)"
SUITE_OPT="-9"                     # stretch — the suite ONL builds armhf under
# Patched, rootless-tolerant builder (non-fatal binfmt mount). Build it once with
# scripts/build-builder.sh.
BUILDER_IMAGE="${BUILDER_IMAGE:-edgenos/builder9:1.8-rootless}"

# --- docker socket selection ---
# EDGENOS_ROOTFUL=1 → use the system (rootful) daemon, where mount/chroot/mknod
# work natively (needed for the rootfs-configure stage). Otherwise rootless.
if [ "${EDGENOS_ROOTFUL:-0}" = "1" ]; then
  : "${DOCKER_HOST:=unix:///var/run/docker.sock}"
else
  : "${DOCKER_HOST:=unix:///run/user/$(id -u)/docker.sock}"
fi
export DOCKER_HOST

MODE="${1:-installer}"

# The build body to run inside the workspace (after sourcing setup.env).
case "$MODE" in
  installer)
    BODY='make armhf' ;;
  packages)
    # canonical per-dir make via pkg.mk — quick compile sanity before full image
    BODY='make -C packages/platforms/accton/armhf/as4610/as4610-54' ;;
  shell)
    BODY='exec bash -i' ;;
  *)
    echo "usage: $0 [installer|packages|shell]" >&2; exit 2 ;;
esac

# NOTE: onlbuilder and the in-container docker_shell both flatten the post-"-c"
# command with a plain " ".join and re-run it through a shell, which destroys
# any quoting/&&/multi-word structure. So we cannot pass a compound command
# inline. Instead, write the real commands to a script file *inside* the mounted
# ONL tree and invoke that single path (survives flattening intact). We also
# avoid `bash -l`, which tries to read a profile the container user can't.
INNER="$ONL/.edgenos-inner.sh"
# The build runs under fakeroot so the initrd/RFS steps can "create" device
# nodes (dev/console etc.) without CAP_MKNOD — impossible in a user namespace.
# fakeroot fakes mknod()/chown() and the repacked cpio gets correct metadata.
# (The sudo shim in the builder image keeps sudo'd children inside this session.)
# 'shell' mode is interactive, so it isn't fakeroot-wrapped. Under rootful Docker
# real root does real mknod/chroot/mount, so fakeroot is unnecessary (and best
# avoided around the chroot-configure stage) — skip it there too.
if [ "$MODE" = "shell" ] || [ "${EDGENOS_ROOTFUL:-0}" = "1" ]; then
  RUN="$BODY"
else
  RUN="fakeroot $BODY"
fi
cat > "$INNER" <<EOF
#!/bin/bash
cd "$ONL"
# The main rootfs is multistrap'd from sources that route through apt-cacher-ng
# at 127.0.0.1:3142 -> archive.debian.org (stretch is EOL but archived). ONL only
# auto-starts the cacher in --isolate mode, so start it ourselves (best-effort).
/etc/init.d/apt-cacher-ng start || true
source setup.env          # tolerate non-zero from its helper invocations
set -e
$RUN
EOF
chmod +x "$INNER"
trap 'rm -f "$INNER"' EXIT

echo "== EdgeNOS-4610 build =="
echo "ONL tree   : $ONL"
echo "builder    : $BUILDER_IMAGE (suite stretch/armhf)"
echo "DOCKER_HOST: $DOCKER_HOST"
echo "mode       : $MODE"
echo "inner      : $INNER"
echo

cd "$ONL"
# onlbuilder mounts $ONL (cwd) + $HOME; the patched builder image makes the
# binfmt_misc mount non-fatal (host F-flag qemu handles armhf exec). The command
# is the single script path — no quoting to lose.
#
# --user root:0 is REQUIRED under rootless Docker: rootless maps host uid 1000 ->
# container root, so the bind-mounted files are owned by container-root. Running
# the build as container-root reads them fine, and (because of the same mapping)
# build outputs land back owned by you on the host — not root-owned.
docker/tools/onlbuilder "$SUITE_OPT" --non-interactive --user root:0 \
     --image "$BUILDER_IMAGE" -c bash "$INNER"
