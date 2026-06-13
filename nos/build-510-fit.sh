#!/usr/bin/env bash
# build-510-fit.sh — reproducibly build the 5.10 loader FIT for the AS4610 Helix4:
#   clean cgroup kernel Image + RTC-disabled DTB + onl-loader-initrd
#   -> output/kport510/arm-accton-as4610-54-r0-510.itb  (byte-reproducible)
# Assumes the kernel Image is already built (see docs/KERNEL_510_OWNBUILD.md §build).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDGE="$(cd "$HERE/../.." && pwd)"
KP="$EDGE/edgecore-4610-54t/output/kport510"
IMG="${BUILDER_IMAGE:-edgenos/builder9:1.8-rootless}"
: "${DOCKER_HOST:=unix:///run/user/$(id -u)/docker.sock}"; export DOCKER_HOST

IMAGE="$KP/linux-5.10.224/arch/arm/boot/Image"
DTS="$EDGE/edgecore-4610-54t/nos/kernel/dts/arm-accton-as4610-rtcdis.dts"   # flattened, verified
ITS="$KP/edgenos-510.its"
for f in "$IMAGE" "$DTS" "$ITS"; do [ -e "$f" ] || { echo "ERROR missing $f" >&2; exit 1; }; done

# Pin SOURCE_DATE_EPOCH so the FIT (mkimage timestamp) is byte-reproducible.
docker run --rm -u root:0 -e SOURCE_DATE_EPOCH=1717480800 -v "$EDGE":"$EDGE" -w "$KP" "$IMG" bash -lc '
  set -e
  dtc -I dts -O dtb '"$DTS"' -o arm-accton-as4610-rtcdis.dtb 2>/dev/null
  gzip -n -9 -c '"$IMAGE"' > kernel-5.10-iproc.bin.gz
  mkimage -f '"$ITS"' arm-accton-as4610-54-r0-510.itb >/dev/null
'
( cd "$KP" && md5sum arm-accton-as4610-54-r0-510.itb )
echo "==> 5.10 FIT: $KP/arm-accton-as4610-54-r0-510.itb"
