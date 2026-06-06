#!/usr/bin/env bash
#
# build-datapath.sh — cross-compile the EdgeNOS-4610 datapath app (OpenMDK
# CDK+BMD+PHY+libbde, scoped to BCM56340) for armhf, using the same toolchain
# (arm-linux-gnueabihf, stretch glibc) as our ONL rootfs so the binary runs on
# the installed NOS.
#
# Compile-only — no privileged ops — so the rootless daemon is fine.
# Produces: mdk-app/linux-user-mdk  (armhf ELF: a CDK/BMD shell to attach the
# chip, init it, bring up ports, poke L2/L3 tables).
#
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDGE="$(cd "$HERE/../../.." && pwd)"       # /home/smiley/edgecore
MDK="$EDGE/OpenMDK"
APP="$HERE/mdk-app"
IMG="${BUILDER_IMAGE:-edgenos/builder9:1.8-rootless}"
: "${DOCKER_HOST:=unix:///run/user/$(id -u)/docker.sock}"; export DOCKER_HOST

[ -d "$MDK/cdk" ] || { echo "OpenMDK not found at $MDK" >&2; exit 1; }

echo "== EdgeNOS-4610 datapath build =="
echo "MDK   : $MDK"
echo "app   : $APP"
echo "image : $IMG (arm-linux-gnueabihf cross)"
echo

# Build inside the builder image (has the cross toolchain + perl for CDK gen).
# Run as container-root (rootless => host user), mount the whole edgecore tree
# so OpenMDK + our app are both visible at their real paths.
docker run --rm -u root:0 \
  -v "$EDGE":"$EDGE" -w "$APP" \
  -e MDK="$MDK" -e CROSS_COMPILE=arm-linux-gnueabihf- \
  "$IMG" bash -lc '
    set -e
    cd "$MDK/examples/linux-user" >/dev/null 2>&1 || true
    cd "'"$APP"'"
    # MAKE=make: the example Makefile hardcodes gmake; the image only has make.
    # CFLAGS without -Werror: this OpenMDK vintage trips warnings-as-errors
    # (e.g. misleading-indentation in the TSCF SerDes driver) under gcc 6.3.
    # We keep -fPIC (the Makefile would otherwise add it via +=, which a
    # command-line CFLAGS suppresses).
    # SYS_BE_*=0: AS4610 (ARM/armhf) is little-endian, so PIO/packet/other bus
    # byte-order flags are all 0 (the app #errors if they are undefined).
    # PHY_CONFIG_INCLUDE_BCM84740=1: register the bcm84740 driver (extended to
    # also claim the 84740-family BCM84758 SFP+ PHY on xe0-3 and load its ucode).
    # PHY_CONFIG_INCLUDE_BCM54282=1: register the bcm54282 driver for the 48x 1G
    # copper RJ-45 ports (octal QSGMII PHYs) so the probe binds + inits them.
    make MAKE=make MDK="$MDK" CROSS_COMPILE=arm-linux-gnueabihf- \
         CFLAGS="-fPIC -Wall -Wno-error -DSYS_BE_PIO=0 -DSYS_BE_PACKET=0 -DSYS_BE_OTHER=0 -DPHY_CONFIG_INCLUDE_BCM84740=1 -DPHY_CONFIG_INCLUDE_BCM54282=1" 2>&1
  '

echo
echo "== result =="
file "$APP/linux-user-mdk" 2>/dev/null || { echo "binary not produced" >&2; exit 1; }
