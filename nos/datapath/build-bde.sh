#!/usr/bin/env bash
#
# build-bde.sh — cross-build OpenBCM's GPL linux-kernel-bde (+ linux_dma/mpool/shbde)
# for the AS4610-54T on-die iProc CMICd, against the ONL 4.14.151 armhf-iproc kernel.
# Produces linux-kernel-bde.ko (vermagic 4.14.151-OpenNetworkLinux-armhf), to be
# loaded on the box where the device-tree node brcm,iproc-cmicd@0x48000000 lets it
# auto-probe the BCM56340. See CPU_PUNT_PLAN.md.
#
set -euo pipefail
EDGE=/home/smiley/edgecore
SDK="$EDGE/edgecore-4610-54t/live-investigation/sdk-ref/sdk-6.5.16/src/gpl-modules"
KERNDIR="$EDGE/OpenNetworkLinux/packages/base/armhf/kernels/kernel-4.14-lts-armhf-iproc-all/builds/stretch/linux-4.14.151"
IMG="${BUILDER_IMAGE:-edgenos/builder9:1.8-rootless}"
: "${DOCKER_HOST:=unix:///run/user/$(id -u)/docker.sock}"; export DOCKER_HOST

[ -f "$SDK/systems/bde/linux/kernel/linux-kernel-bde.c" ] || { echo "BDE src missing at $SDK" >&2; exit 1; }
[ -f "$KERNDIR/Module.symvers" ] || { echo "kernel tree not prepared at $KERNDIR" >&2; exit 1; }

echo "== linux-kernel-bde cross-build (armhf / iproc-cmicd) =="
echo "SDK     : $SDK"
echo "KERNDIR : $KERNDIR"
echo "image   : $IMG"

docker run --rm -u root:0 \
  -v "$EDGE":"$EDGE" -w "$SDK/systems/bde/linux/kernel" \
  "$IMG" bash -lc '
    set -e
    export ARCH=arm
    export CROSS_COMPILE=arm-linux-gnueabihf-
    KINC=$(arm-linux-gnueabihf-gcc -print-file-name=include)
    # gpl-modules/include is a GPL-curated subset missing only soc/drv.h, which
    # linux-kernel-bde.c barely uses. We supply a minimal shim soc/drv.h via bde-shim
    # instead of the full SDK include tree whose sal/core/libc.h has K&R defs that
    # fail under the kernel strict -Werror -nostdinc flags. Shim dir searched first.
    SHIM="'"$SDK"'/../../../bde-shim"
    make \
      SDK="'"$SDK"'" \
      platform=iproc \
      KERNDIR="'"$KERNDIR"'" \
      LINUX_INCLUDE="'"$KERNDIR"'/include" \
      CROSS_COMPILE=arm-linux-gnueabihf- \
      TARGET_ARCHITECTURE=arm-linux-gnueabihf \
      TOOLCHAIN_BASE_DIR=/usr \
      KFLAG_INCLD="$KINC" \
      kernel_version=4_4 \
      INCFLAGS="-I$SHIM -I'"$SDK"'/include -I'"$SDK"'/systems" \
      2>&1
  ' 2>&1 | tail -60

echo
echo "== result =="
find "$SDK" -name "linux-kernel-bde.ko" -newer "$SDK/systems/bde/linux/kernel/linux-kernel-bde.c" 2>/dev/null | while read -r ko; do
  echo "$ko"; file "$ko"
  docker run --rm -u root:0 -v "$EDGE":"$EDGE" "$IMG" bash -lc "modinfo '$ko' 2>/dev/null | grep -iE 'vermagic|license|name'" 2>/dev/null || true
done
