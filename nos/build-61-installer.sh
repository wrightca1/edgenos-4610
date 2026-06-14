#!/usr/bin/env bash
# build-510-installer.sh — reproducible EdgeNOS-4610 / Linux-6.1 onie-installer
#                          for the PURE Buildroot OWN-BUILD NOS.
#
# Produces a self-extracting ONIE installer (ONL mkshar/sfx) that, on a fresh or
# reflashed AS4610-54T, installs:
#   - kernel  : clean 6.1 FIT = forward-ported 6.1.175 (cgroups) + RTC-disabled DTB
#               (-> /mnt/onl/boot/onl-loader-fit.itb)   [build-510-fit.sh]
#   - rootfs  : EdgeNOS-4610-61-ownbuild.swi = pure Buildroot (glibc+systemd, armhf
#               hard-float) + ko510 lockstep with the kernel + datapath autostart
#               (-> /mnt/onl/images)                    [build-ownbuild-swi.sh]
#   - boot-config : SWI=images:EdgeNOS-4610-61-ownbuild.swi  (explicit, NOT ::latest)
# i.e. a cold boot comes up running our OWN NOS (uname 5.10, hostname edgenos-4610),
# datapath autostarting (bcmd + L3 + Quagga), 0% ping both ports.
#
# Method: identical to build-419f-installer.sh — reuse ONL's installer.sh/sfx from the
# stock installer payload verbatim, swapping only the loader FIT, the SWI, boot-config,
# and the loader-initrd offset/size (carved out of the FIT by installer.sh via dd; the
# offsets differ per-FIT, so recompute with pyfit). mkshar/zip/dtc run in the container.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDGE="$(cd "$HERE/../.." && pwd)"                       # /home/smiley/edgecore
T="$EDGE/edgecore-4610-54t"
ONL="$EDGE/OpenNetworkLinux"
IMG="${BUILDER_IMAGE:-edgenos/builder9:1.8-rootless}"
: "${DOCKER_HOST:=unix:///run/user/$(id -u)/docker.sock}"; export DOCKER_HOST

FIT="$T/output/kport61/arm-accton-as4610-54-r0-61.itb"      # clean 6.1 (cgroup) FIT
SWI="$T/output/kport61/EdgeNOS-4610-61-ownbuild.swi"        # pure Buildroot own-build SWI
STOCK_INST="$T/output/onie-installer-edgenos-419"            # source of installer.sh/sfx payload (generic)
OUT="$T/output/onie-installer-edgenos-61-ownbuild"
WORK="$T/output/inst61-build"             # under $EDGE so it's visible in the build container

for f in "$FIT" "$SWI" "$STOCK_INST" "$ONL/tools/scripts/sfx.sh.in"; do
  [ -e "$f" ] || { echo "ERROR missing $f" >&2; exit 1; }
done

# --- stage the payload from the stock installer, then swap in our bits ---
rm -rf "$WORK"; mkdir -p "$WORK/src"
# unzip warns (exit 1) about the SFX sh-prefix offset but still extracts fine; tolerate it.
unzip -q "$STOCK_INST" -d "$WORK/src" || true
[ -f "$WORK/src/installer.sh" ] || { echo "ERROR: payload extract failed" >&2; exit 1; }
mkdir -p "$WORK/stage"
cp "$WORK/src/installer.sh" "$WORK/src/autoperms.sh" \
   "$WORK/src/preinstall.sh" "$WORK/src/postinstall.sh" "$WORK/stage/"
cp -r "$WORK/src/config" "$WORK/src/plugins" "$WORK/stage/"
cp "$FIT" "$WORK/stage/onl-loader-fit.itb"
cp "$SWI" "$WORK/stage/$(basename "$SWI")"
printf 'NETDEV=ma1\nBOOTMODE=SWI\nSWI=images:%s\n' "$(basename "$SWI")" > "$WORK/stage/boot-config"

# --- recompute the loader-initrd offset/size for the 6.1 FIT and patch installer.sh ---
VONL="$ONL/packages/base/all/vendor-config-onl"
read -r OFF LAST < <(docker run --rm -u root:0 -v "$EDGE":"$EDGE" "$IMG" bash -lc \
  "PYTHONPATH=$VONL/src/python python2 $VONL/src/bin/pyfit offset '$FIT' --initrd 2>/dev/null")
SIZE=$(( LAST - OFF + 1 ))    # pyfit prints first/last byte indices (inclusive)
echo "6.1 FIT initrd: offset=$OFF size=$SIZE"
sed -i -e "s/^initrd_offset=.*/initrd_offset=\"$OFF\"/" \
       -e "s/^initrd_size=.*/initrd_size=\"$SIZE\"/" "$WORK/stage/installer.sh"
grep -E '^initrd_(offset|size)=' "$WORK/stage/installer.sh"

# --- repackage with mkshar (same args ONL's mkinstaller uses) ---
docker run --rm -u root:0 -v "$EDGE":"$EDGE" -w "$WORK/stage" "$IMG" bash -lc "
  set -e
  python2 '$ONL/tools/mkshar' --lazy --unzip-pad --fixup-perms autoperms.sh \
    out.shar '$ONL/tools/scripts/sfx.sh.in' installer.sh \
    onl-loader-fit.itb '$(basename "$SWI")' boot-config preinstall.sh postinstall.sh \
    config plugins
"
cp "$WORK/stage/out.shar" "$OUT"
chmod +x "$OUT"
( cd "$(dirname "$OUT")" && md5sum "$(basename "$OUT")" > "$(basename "$OUT").md5sum" )
echo "==> built $OUT"; ls -lh "$OUT"; cat "$OUT.md5sum"
