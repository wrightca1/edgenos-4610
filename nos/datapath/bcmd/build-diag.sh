#!/usr/bin/env bash
# build-diag.sh — build STOCK SDK bcm.user (full interactive diag shell, no
# bcmd_run diversion) for AS4610 RX-lock investigation. Same docker/make recipe
# as build-bcmd.sh; just doesn't patch socdiag.c.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDGE=/home/smiley/edgecore
SDK="$EDGE/edgecore-4610-54t/live-investigation/sdk-ref/OpenBCM/sdk-6.5.16"
KERNDIR="$EDGE/OpenNetworkLinux/packages/base/armhf/kernels/kernel-4.14-lts-armhf-iproc-all/builds/stretch/linux-4.14.151"
IMG="${BUILDER_IMAGE:-edgenos/builder9:1.8-rootless}"
: "${DOCKER_HOST:=unix:///run/user/$(id -u)/docker.sock}"; export DOCKER_HOST
BCMUSER="$SDK/build/linux/user/iproc-4_4/bcm.user"
# ensure socdiag.c is pristine (no leftover bcmd diversion)
SOCDIAG="$SDK/systems/linux/user/common/socdiag.c"
[ -f "$SOCDIAG.bcmdbak" ] && mv -f "$SOCDIAG.bcmdbak" "$SOCDIAG" && echo "restored socdiag.c from bak"
grep -q "bcmd_run();" "$SOCDIAG" && { echo "socdiag.c still diverted! aborting" >&2; exit 1; }
rm -f "$SDK"/build/*/user/iproc-4_4/socdiag.o "$BCMUSER" "$BCMUSER.dbg" 2>/dev/null || true
echo "== building stock bcm.user =="
docker run --rm -u root:0 \
  -v "$SDK":/sdk -v "$KERNDIR":/kern:ro -w /sdk/systems/linux/user/iproc-4_4 \
  "$IMG" bash -lc '
    set -e
    KINC=$(arm-linux-gnueabihf-gcc -print-file-name=include)
    ADD_TO_CFLAGS="-Wno-error -Wno-cpp -DINCLUDE_KNET -I/sdk/systems/linux/kernel/modules/include" \
    make SDK=/sdk CROSS_COMPILE=arm-linux-gnueabihf- KERNDIR=/kern \
         TOOLCHAIN_BASE_DIR=/usr KFLAG_INCLD="$KINC" LINUX_MAKE_USER=1 \
         BUILD_KNET=1 MAKE=make -j6 bcm 2>&1
  ' 2>&1 | tail -15
[ -f "$BCMUSER" ] || { echo "build did not produce bcm.user" >&2; exit 1; }
cp -f "$BCMUSER" "$HERE/bcm.user"
docker run --rm -u root:0 -v "$EDGE":"$EDGE" "$IMG" arm-linux-gnueabihf-strip "$HERE/bcm.user" 2>/dev/null || true
echo "== result =="; file "$HERE/bcm.user"
