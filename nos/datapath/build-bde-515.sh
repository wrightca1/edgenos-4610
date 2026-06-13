#!/usr/bin/env bash
#
# build-bde.sh — cross-build OpenBCM's GPL kernel BDE modules for the AS4610-54T
# on-die iProc CMICd, against the ONL 4.14.151 armhf-iproc kernel:
#   linux-kernel-bde.ko  — device enumerator + coherent DMA pool (+ linux_dma/mpool/shbde)
#   linux-user-bde.ko    — userspace LUBDE_* ioctl interface (GET_DMA_INFO + mmap)
# Both vermagic 4.14.151-OpenNetworkLinux-armhf. On the box the device-tree node
# brcm,iproc-cmicd@0x48000000 lets the BDE auto-probe the BCM56340 (after unbinding
# the built-in iproc_cmic). See CPU_PUNT_PLAN.md. Staged into artifacts/.
#
set -euo pipefail
EDGE=/home/smiley/edgecore
SDK="$EDGE/edgecore-4610-54t/live-investigation/sdk-ref/sdk-6.5.16/src/gpl-modules"
KERNDIR="$EDGE/edgecore-4610-54t/output/kport515/linux-5.15.209"
IMG="${BUILDER_IMAGE:-edgenos/builder9:1.8-rootless}"
ART="$EDGE/edgecore-4610-54t/output/kport515/ko515"
: "${DOCKER_HOST:=unix:///run/user/$(id -u)/docker.sock}"; export DOCKER_HOST

[ -f "$SDK/systems/bde/linux/kernel/linux-kernel-bde.c" ] || { echo "BDE src missing at $SDK" >&2; exit 1; }
[ -f "$KERNDIR/Module.symvers" ] || { echo "kernel tree not prepared at $KERNDIR" >&2; exit 1; }

echo "== kernel BDE cross-build (armhf / iproc-cmicd) =="
echo "SDK     : $SDK"
echo "KERNDIR : $KERNDIR"

# Build one BDE module dir ($1 = relative dir under systems/bde/linux). The kernel-bde
# build emits Module.symvers (exports lkbde_*); pass it to the user-bde build via
# KBUILD_EXTRA_SYMBOLS so modpost resolves lkbde_get_dma_info cleanly.
build_mod() {
  local subdir="$1" extrasym="$2"
  docker run --rm -u root:0 \
    -v "$EDGE":"$EDGE" -w "$SDK/systems/bde/linux/$subdir" \
    "$IMG" bash -lc '
      set -e
      export ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-
      KINC=$(arm-linux-gnueabihf-gcc -print-file-name=include)
      # gpl-modules/include is a GPL-curated subset missing only soc/drv.h, which the
      # bde sources barely use. We supply a minimal shim soc/drv.h via bde-shim instead
      # of the full SDK include tree whose sal/core/libc.h has K&R defs that fail under
      # the kernel strict -Werror -nostdinc flags. Shim dir searched first.
      SHIM="'"$SDK"'/../../../bde-shim"
      EXTRA="'"$extrasym"'"
      make \
        SDK="'"$SDK"'" platform=iproc \
        KERNDIR="'"$KERNDIR"'" LINUX_INCLUDE="'"$KERNDIR"'/include" \
        CROSS_COMPILE=arm-linux-gnueabihf- TARGET_ARCHITECTURE=arm-linux-gnueabihf \
        TOOLCHAIN_BASE_DIR=/usr KFLAG_INCLD="$KINC" kernel_version=4_4 \
        INCFLAGS="-I$SHIM -I'"$SDK"'/include -I'"$SDK"'/systems" \
        ${EXTRA:+KBUILD_EXTRA_SYMBOLS="$EXTRA"} \
        2>&1
    ' 2>&1 | tail -25
}

KSYMS="$SDK/build/linux-iproc/systems/bde/linux/kernel/kernel_module/Module.symvers"

echo "--- [1/2] linux-kernel-bde ---"
build_mod kernel ""
echo "--- [2/2] linux-user-bde ---"
build_mod user/kernel "$KSYMS"

echo
echo "== result =="
mkdir -p "$ART"
for ko in linux-kernel-bde linux-user-bde; do
  f=$(find "$SDK/build" -name "$ko.ko" 2>/dev/null | head -1)
  if [ -n "$f" ]; then
    cp "$f" "$ART/"
    echo "$ko.ko -> artifacts/"
    docker run --rm -u root:0 -v "$EDGE":"$EDGE" "$IMG" bash -lc "modinfo '$f' 2>/dev/null | grep -iE 'name|license|vermagic'" 2>/dev/null | grep -ivE "ttyname|^mesg" || true
  else
    echo "$ko.ko : NOT BUILT"
  fi
done
