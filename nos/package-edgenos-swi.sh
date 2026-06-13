#!/usr/bin/env bash
# package-edgenos-swi.sh — bake the EdgeNOS-4610 datapath into a built ONL SWI.
#
# Takes the SWI produced by `make armhf` (kernel already has our config patch:
# CONFIG_IP_ROUTE_MULTIPATH etc.) and overlays our datapath + control-plane into
# its rootfs so everything comes up on boot — the "Step 0 foundation" image.
# Same robust approach as the AS5610 assemble-rootfs-from-base (unsquash + overlay
# + re-squash), but on the SWI artifact, so it's independent of ONL package YAML.
#
# Baked layout (rootfs, so it persists — it IS the image):
#   /opt/edgenos/{bcmd,*.ko,config.bcm,zebra-arm,ospfd-arm,bcmd-prep.sh,edgenos-l3-config.sh}
#   /etc/edged/{addrs,routes}.conf   /etc/quagga/{zebra,ospfd}.conf
#   /etc/systemd/system/{bcmd,edgenos-l3,zebra,ospfd}.service  (enabled)
#
# The SWI is a zip{rootfs-armhf.sqsh, manifest.json}; manifest carries NO rootfs
# checksum, so we just re-squash + rezip. squashfs tools run in the builder container.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDGE="$HERE/.."                                   # edgecore-4610-54t
SWI_IN="${1:?usage: package-edgenos-swi.sh <input.swi> <output.swi>}"
SWI_OUT="${2:?output swi path}"
IMG="${BUILDER_IMAGE:-edgenos/builder9:1.8-rootless}"
: "${DOCKER_HOST:=unix:///run/user/$(id -u)/docker.sock}"; export DOCKER_HOST

DP="$EDGE/nos/datapath/artifacts"                 # bcmd + 3 .ko
CFG="$EDGE/nos/datapath/config.bcm.as4610-54t"
QG="$EDGE/nos/datapath/routing"                   # zebra-arm, ospfd-arm
OVL="$EDGE/nos/datapath/rootfs-overlay"

for f in "$DP/bcmd" "$DP/linux-kernel-bde.ko" "$DP/linux-user-bde.ko" "$DP/linux-bcm-knet.ko" \
         "$CFG" "$QG/zebra-arm" "$QG/ospfd-arm" "$OVL/etc/edged/addrs.conf"; do
    [ -f "$f" ] || { echo "ERROR: missing $f" >&2; exit 1; }
done

STAGE=/tmp/edgenos-swi-stage
rm -rf "$STAGE"; mkdir -p "$STAGE/in"
cp "$SWI_IN" "$STAGE/in/in.swi"
mkdir -p "$STAGE/dp"; cp "$DP"/bcmd "$DP"/*.ko "$CFG" "$QG/zebra-arm" "$QG/ospfd-arm" "$STAGE/dp/"
cp "$CFG" "$STAGE/dp/config.bcm"
# Stream the overlay into the container via STDIN, not the bind mount: rootless
# docker's bind mount of the freshly-written stage intermittently serves a stale
# view of just-created files (init.d/, rc.local — the newest, at the tar tail),
# yielding ENOENT/EACCES inside the container. Piping the tar over stdin (host
# reads its own files consistently) sidesteps that entirely; in.swi + dp/ stay on
# the bind mount (large pre-staged files read fine).
tar -C "$OVL" -cf - . | docker run -i --rm -u root:0 -v "$STAGE:/work" --entrypoint /bin/bash "$IMG" -c '
set -e
cd /work
# Consume the stdin-streamed overlay tar FIRST, before unzip/unsquashfs run —
# those drain/close stdin and would truncate the tar tail (init.d/, rc.local).
rm -rf /root/ovl && mkdir -p /root/ovl
# --no-same-owner/--no-same-permissions: extract as container-root with sane
# perms. Preserving the archived uid 1000 yields files owned by an unmapped
# subuid; rootless fuse-overlayfs then denies even container-root (no working
# CAP_DAC_OVERRIDE for subuid files) once a dir is not world-accessible.
tar -C /root/ovl -xf - --no-same-owner --no-same-permissions

unzip -o -q in/in.swi -d ex            # -> ex/rootfs-armhf.sqsh, ex/manifest.json
unsquashfs -n -no-xattrs -d rfs ex/rootfs-armhf.sqsh >/dev/null

# --- bake datapath under /opt/edgenos (rootfs => persistent) ---
mkdir -p rfs/opt/edgenos rfs/etc/edged rfs/etc/quagga rfs/etc/init.d rfs/etc/systemd/system/multi-user.target.wants

# Read EVERY overlay file out of /root/ovl in one consecutive block, FIRST.
# Rootless fuse-overlayfs loses coherence on /root/ovl after later unrelated ops
# (seds, time), denying even container-root; reading it all up front sidesteps that.
cp /root/ovl/usr/sbin/bcmd-prep.sh /root/ovl/usr/sbin/edgenos-l3-config.sh rfs/opt/edgenos/
cp /root/ovl/etc/edged/*.conf  rfs/etc/edged/
cp /root/ovl/etc/quagga/*.conf rfs/etc/quagga/
cp /root/ovl/etc/init.d/edgenos rfs/etc/init.d/edgenos
cp /root/ovl/etc/rc.local       rfs/etc/rc.local
for u in bcmd edgenos-l3 zebra ospfd; do
  cp /root/ovl/etc/systemd/system/$u.service rfs/etc/systemd/system/$u.service
done

# Heavy binaries (no more /root/ovl reads past this point).
cp dp/bcmd dp/*.ko dp/config.bcm dp/zebra-arm dp/ospfd-arm rfs/opt/edgenos/
chmod +x rfs/opt/edgenos/bcmd rfs/opt/edgenos/*-arm rfs/opt/edgenos/*.sh rfs/etc/init.d/edgenos rfs/etc/rc.local

# --- retarget paths to the baked layout (all edits on rfs, no /root/ovl reads) ---
# bcmd-prep loads .ko from $D=/mnt/onl/data -> /opt/edgenos
sed -i "s#/mnt/onl/data#/opt/edgenos#g" rfs/opt/edgenos/bcmd-prep.sh

# Autostart on sysvinit ONL via rc.local (THE working hook): ONL ARM is sysvinit +
# startpar concurrent boot, which honors insserv'\''s dependency DB (.depend.start),
# NOT hand-placed rc?.d/SXX symlinks (silently skipped). /etc/rc.local IS in that DB
# and runs late (after network). /etc/init.d/edgenos is the start/stop/status impl.
echo "==> baked autostart: /etc/rc.local -> /etc/init.d/edgenos start"

# systemd units too (inert on sysvinit ONL, kept for forward-compat)
for u in bcmd edgenos-l3 zebra ospfd; do
  sed -i -e "s#/mnt/onl/data#/opt/edgenos#g" \
         -e "s#/usr/sbin/bcmd-prep.sh#/opt/edgenos/bcmd-prep.sh#g" \
         -e "s#/usr/sbin/edgenos-l3-config.sh#/opt/edgenos/edgenos-l3-config.sh#g" \
         -e "s#/usr/sbin/zebra-arm#/opt/edgenos/zebra-arm#g" \
         -e "s#/usr/sbin/ospfd-arm#/opt/edgenos/ospfd-arm#g" \
         rfs/etc/systemd/system/$u.service
  ln -sf ../$u.service rfs/etc/systemd/system/multi-user.target.wants/$u.service
done

# --- re-squash + repackage the SWI (same zip layout; manifest unchanged) ---
rm -f ex/rootfs-armhf.sqsh
mksquashfs rfs ex/rootfs-armhf.sqsh -noappend -no-xattrs -comp xz >/dev/null
( cd ex && zip -q -X ../out.swi rootfs-armhf.sqsh manifest.json )
echo "==> EdgeNOS SWI built: $(du -h /work/out.swi | cut -f1)"
'
cp "$STAGE/out.swi" "$SWI_OUT"
( cd "$(dirname "$SWI_OUT")" && md5sum "$(basename "$SWI_OUT")" > "$(basename "$SWI_OUT").md5sum" )
echo "==> EdgeNOS-4610 SWI: $SWI_OUT"
ls -lh "$SWI_OUT"
