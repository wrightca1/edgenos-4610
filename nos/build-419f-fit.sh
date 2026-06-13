#!/usr/bin/env bash
# build-419f-fit.sh — reproducibly build the 419f loader FIT from source artifacts:
#   kernel Image (clock-fixed 4.19.81) + RTC-disabled DTB (compiled from the checked-in
#   canonical flat DTS) + onl-loader-initrd  -> arm-accton-as4610-54-r0-419f.itb
#
# The "nice" board DTS (nos/kernel/dts/arm-accton-as4610.dts) #includes bcm-helix4.dtsi
# which isn't vendored standalone, so the FIT-DTB build input is the flattened, verified
# DTS (nos/kernel/dts/arm-accton-as4610-rtcdis.dts) decompiled from the working ONL
# platform DTB with rtc@68 disabled — it dtc's to the exact DTB proven on hardware.
# Runs dtc/gzip/mkimage in the build container.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDGE="$(cd "$HERE/../.." && pwd)"
T="$EDGE/edgecore-4610-54t"; KP="$T/output/kport419"
IMG="${BUILDER_IMAGE:-edgenos/builder9:1.8-rootless}"
: "${DOCKER_HOST:=unix:///run/user/$(id -u)/docker.sock}"; export DOCKER_HOST

IMAGE="$KP/linux-4.19.81/arch/arm/boot/Image"      # built by the kernel build (clock-fixed)
DTS="$T/nos/kernel/dts/arm-accton-as4610-rtcdis.dts"
ITS="$KP/edgenos-419f.its"
for f in "$IMAGE" "$DTS" "$ITS"; do [ -e "$f" ] || { echo "ERROR missing $f" >&2; exit 1; }; done

# Pin the FIT/gzip timestamp so the build is byte-reproducible (mkimage otherwise
# stamps the current time into the FIT 'timestamp' property -> md5 drifts each build).
SDE=1717480800   # 2024-06-04 (fixed, arbitrary but stable)
docker run --rm -u root:0 -e SOURCE_DATE_EPOCH="$SDE" -v "$EDGE":"$EDGE" -w "$KP" "$IMG" bash -lc '
  set -e
  dtc -I dts -O dtb '"$DTS"' -o arm-accton-as4610-rtcdis.dtb 2>/dev/null
  gzip -n -9 -c '"$IMAGE"' > kernel-4.19-iproc.bin.gz
  mkimage -f '"$ITS"' arm-accton-as4610-54-r0-419f.itb >/dev/null
'
( cd "$KP" && md5sum arm-accton-as4610-rtcdis.dtb arm-accton-as4610-54-r0-419f.itb )
echo "==> 419f FIT: $KP/arm-accton-as4610-54-r0-419f.itb"
