#!/usr/bin/env bash
# build-ownbuild-swi.sh — assemble the EdgeNOS-4610 Buildroot/5.10 own-build SWI,
# REBUILDING the GPL BDE/KNET modules against the current 5.10 kernel tree first.
#
# WHY the module rebuild is in here: CONFIG_MEMCG/CGROUPS change struct page/task_struct,
# so ko510 built against a different kernel config fault in KNET dma_map_single even though
# vermagic matches. Whenever the kernel is rebuilt, the modules MUST be rebuilt too — this
# script guarantees they're in lockstep, then bakes them into the rootfs squashfs.
#
# Prereqs (the long, separate steps — see docs/KERNEL_510_OWNBUILD.md):
#   - kernel built:   output/kport510/linux-5.10.224/{arch/arm/boot/Image,Module.symvers}
#   - SDK patched:    live-investigation/.../sdk-6.5.16 (sdk-6.5.16-linux5.10-compat.patch applied)
#   - buildroot rootfs built: ownbuild/build/buildroot-2023.02.9/output/images/rootfs.squashfs
#   - bcmd in nos/datapath/artifacts510/ ; manifest.json in output/kport510/
# Output: output/kport510/EdgeNOS-4610-510-ownbuild.swi
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"        # nos/
EDGE="$(cd "$HERE/.." && pwd)"                              # edgecore-4610-54t
ROOT="$(cd "$EDGE/.." && pwd)"                              # /home/smiley/edgecore
IMG="${BUILDER_IMAGE:-edgenos/builder9:1.8-rootless}"
: "${DOCKER_HOST:=unix:///run/user/$(id -u)/docker.sock}"; export DOCKER_HOST
SDK="$EDGE/live-investigation/sdk-ref/sdk-6.5.16/src/gpl-modules"
K510="$EDGE/output/kport510/linux-5.10.224"
KO="$EDGE/output/kport510/ko510"
KP="$EDGE/output/kport510"
SQ="$EDGE/ownbuild/build/buildroot-2023.02.9/output/images/rootfs.squashfs"

[ -f "$K510/Module.symvers" ] || { echo "ERROR: kernel not built at $K510" >&2; exit 1; }
[ -f "$SQ" ] || { echo "ERROR: buildroot rootfs not built at $SQ" >&2; exit 1; }

echo "== [1/3] rebuild BDE/KNET (ko510) against the current 5.10 kernel =="
bash "$HERE/datapath/build-bde-510.sh"                      # kernel-bde + user-bde -> ko510
# knet (built directly with target=linux-iproc; KBUILD_EXTRA_SYMBOLS propagated to inner kbuild)
rm -rf "$SDK/build/linux-iproc/systems/linux/kernel/modules/bcm-knet/kernel_module"
docker run --rm -u root:0 -v "$ROOT":"$ROOT" -w "$SDK/systems/linux/kernel/modules/bcm-knet" "$IMG" bash -lc '
  set -e; export ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-
  KINC=$(arm-linux-gnueabihf-gcc -print-file-name=include); SHIM="'"$SDK"'/../../../bde-shim"
  make target=linux-iproc SDK="'"$SDK"'" platform=iproc KERNDIR="'"$K510"'" LINUX_INCLUDE="'"$K510"'/include" \
    CROSS_COMPILE=arm-linux-gnueabihf- TARGET_ARCHITECTURE=arm-linux-gnueabihf TOOLCHAIN_BASE_DIR=/usr \
    KFLAG_INCLD="$KINC" kernel_version=4_4 INCFLAGS="-I$SHIM -I'"$SDK"'/include -I'"$SDK"'/systems"' >/dev/null 2>&1
cp "$(find "$SDK/build" -name linux-bcm-knet.ko | head -1)" "$KO/"
echo "   ko510:"; for k in "$KO"/*.ko; do docker run --rm -u root:0 -v "$ROOT":"$ROOT" "$IMG" bash -lc "modinfo '$k' 2>/dev/null|grep -E '^filename|vermagic'" 2>/dev/null|grep -iv ttyname; done

echo "== [2/3] bake ko510 into the buildroot rootfs squashfs =="
cp "$KO"/*.ko "$EDGE/ownbuild/config/overlay/opt/edgenos/"   # keep overlay in sync for full rebuilds
docker run --rm -u root:0 -v "$ROOT":"$ROOT" -w "$KP" "$IMG" bash -lc '
  set -e; rm -rf rootfs-rw
  unsquashfs -n -d rootfs-rw '"$SQ"' >/dev/null
  cp '"$KO"'/*.ko rootfs-rw/opt/edgenos/
  rm -f rootfs-armhf.sqsh
  mksquashfs rootfs-rw rootfs-armhf.sqsh -noappend -no-xattrs -comp xz >/dev/null
  rm -rf rootfs-rw'

echo "== [3/3] wrap as ONL SWI (zip rootfs-armhf.sqsh + manifest.json) =="
[ -f "$KP/manifest.json" ] || ( cd "$KP" && unzip -o -q EdgeNOS-4610-510-auto.swi manifest.json 2>/dev/null || true )
docker run --rm -u root:0 -v "$ROOT":"$ROOT" -w "$KP" "$IMG" bash -lc '
  rm -f EdgeNOS-4610-510-ownbuild.swi
  zip -X -q -n .sqsh:.swi EdgeNOS-4610-510-ownbuild.swi rootfs-armhf.sqsh manifest.json
  rm -f rootfs-armhf.sqsh'
( cd "$KP" && md5sum EdgeNOS-4610-510-ownbuild.swi | tee EdgeNOS-4610-510-ownbuild.swi.md5 )
echo "==> own-build SWI: $KP/EdgeNOS-4610-510-ownbuild.swi  (ko510 lockstep with the kernel)"
