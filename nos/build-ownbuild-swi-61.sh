#!/usr/bin/env bash
# build-ownbuild-swi-61.sh — assemble the EdgeNOS-4610 Buildroot/6.1 own-build SWI.
# ko61 is already built + HW-validated (see docs), so unlike build-ownbuild-swi.sh
# this skips the module rebuild and just bakes ko61 into the (kernel-independent)
# buildroot rootfs squashfs, then wraps the ONL SWI.
# Output: output/kport61/EdgeNOS-4610-61-ownbuild.swi
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"        # nos/
EDGE="$(cd "$HERE/.." && pwd)"                              # edgecore-4610-54t
ROOT="$(cd "$EDGE/.." && pwd)"                              # /home/smiley/edgecore
IMG="${BUILDER_IMAGE:-edgenos/builder9:1.8-rootless}"
: "${DOCKER_HOST:=unix:///run/user/$(id -u)/docker.sock}"; export DOCKER_HOST
KO="$EDGE/output/kport61/ko61"
KP="$EDGE/output/kport61"
SQ="$EDGE/ownbuild/build/buildroot-2023.02.9/output/images/rootfs.squashfs"

[ -f "$KO/linux-kernel-bde.ko" ] || { echo "ERROR: ko61 not built at $KO" >&2; exit 1; }
[ -f "$SQ" ] || { echo "ERROR: buildroot rootfs not built at $SQ" >&2; exit 1; }
[ -f "$KP/manifest.json" ] || cp "$EDGE/output/kport510/manifest.json" "$KP/manifest.json"

echo "== bake ko61 into the buildroot rootfs squashfs =="
cp "$KO"/*.ko "$EDGE/ownbuild/config/overlay/opt/edgenos/"   # keep overlay in sync
docker run --rm -u root:0 -v "$ROOT":"$ROOT" -w "$KP" "$IMG" bash -lc '
  set -e; rm -rf rootfs-rw
  unsquashfs -n -d rootfs-rw '"$SQ"' >/dev/null
  cp '"$KO"'/*.ko rootfs-rw/opt/edgenos/
  # re-sync the canonical overlay systemd units (the buildroot squashfs may predate
  # overlay edits — e.g. the version-neutral bcmd.service Description) so a SWI repack
  # always matches source without a full buildroot rebuild.
  cp '"$EDGE"'/ownbuild/config/overlay/etc/systemd/system/*.service rootfs-rw/etc/systemd/system/ 2>/dev/null || true
  rm -f rootfs-armhf.sqsh
  mksquashfs rootfs-rw rootfs-armhf.sqsh -noappend -no-xattrs -comp xz >/dev/null
  rm -rf rootfs-rw'

echo "== wrap as ONL SWI (zip rootfs-armhf.sqsh + manifest.json) =="
docker run --rm -u root:0 -v "$ROOT":"$ROOT" -w "$KP" "$IMG" bash -lc '
  rm -f EdgeNOS-4610-61-ownbuild.swi
  zip -X -q -n .sqsh:.swi EdgeNOS-4610-61-ownbuild.swi rootfs-armhf.sqsh manifest.json
  rm -f rootfs-armhf.sqsh'
( cd "$KP" && md5sum EdgeNOS-4610-61-ownbuild.swi | tee EdgeNOS-4610-61-ownbuild.swi.md5 )
echo "==> 6.1 own-build SWI: $KP/EdgeNOS-4610-61-ownbuild.swi  (ko61 baked)"
