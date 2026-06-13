#!/usr/bin/env bash
#
# build-bcmd.sh — build the EdgeNOS-4610 datapath daemon `bcmd` from the full
# OpenBCM SDK (sdk-6.5.16) for armhf / iProc-CMICd.
#
# Strategy (low-risk reuse of the proven bcm.user build): the daemon body lives
# in bcmd.c, which we APPEND to the SDK's socdiag.c and divert its REPL —
# `diag_shell();` -> `bcmd_run();`. socdiag.c keeps providing main(), bde_create()
# and all the BDE/PCI/KNET platform hooks the SDK libs need; we only swap the
# interactive shell for our deterministic datapath. The exact same `make bcm`
# (INCLUDE_KNET + BUILD_KNET) recipe that produced bcm.user is reused — libs are
# already built, so this is an incremental recompile of socdiag.o + relink.
#
# Output: bcmd (armhf ELF), staged next to this script and into artifacts/.
# socdiag.c is restored on exit (trap), so the SDK tree is left pristine.
#
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDGE=/home/smiley/edgecore
SDK="$EDGE/edgecore-4610-54t/live-investigation/sdk-ref/OpenBCM/sdk-6.5.16"
KERNDIR="$EDGE/edgecore-4610-54t/output/kport419/linux-4.19.81"
IMG="${BUILDER_IMAGE:-edgenos/builder9:1.8-rootless}"
ART="$EDGE/edgecore-4610-54t/output/kport419/ko419"
: "${DOCKER_HOST:=unix:///run/user/$(id -u)/docker.sock}"; export DOCKER_HOST

SOCDIAG="$SDK/systems/linux/user/common/socdiag.c"
PLATDIR="$SDK/systems/linux/user/iproc-4_4"
BCMUSER="$SDK/build/linux/user/iproc-4_4/bcm.user"

[ -f "$SOCDIAG" ]    || { echo "socdiag.c not found at $SOCDIAG" >&2; exit 1; }
[ -d "$PLATDIR" ]    || { echo "platform dir missing at $PLATDIR" >&2; exit 1; }
[ -f "$HERE/bcmd.c" ]|| { echo "bcmd.c missing" >&2; exit 1; }
ls "$SDK"/build/unix-user/iproc-4_4/libbcm.a >/dev/null 2>&1 \
    || { echo "SDK libs not built yet — run the bcm.user build first" >&2; exit 1; }

# --- patch socdiag.c (with restore trap) ---
BAK="$SOCDIAG.bcmdbak"
restore() { [ -f "$BAK" ] && mv -f "$BAK" "$SOCDIAG" && echo "[bcmd] socdiag.c restored"; }
trap restore EXIT
cp -f "$SOCDIAG" "$BAK"

if ! grep -q "diag_shell();" "$SOCDIAG"; then
    echo "diag_shell(); call not found in socdiag.c — aborting" >&2; exit 1
fi
# divert the REPL to our datapath, and pull in the daemon body
sed -i 's/diag_shell();/bcmd_run();/' "$SOCDIAG"
{ echo ""; echo "/* ==== appended by build-bcmd.sh: EdgeNOS-4610 datapath ==== */";
  cat "$HERE/bcmd.c"; } >> "$SOCDIAG"
echo "[bcmd] socdiag.c patched (diag_shell -> bcmd_run, bcmd.c appended)"

# force socdiag.o + bcm.user rebuild (libs untouched -> fast incremental relink)
rm -f "$SDK"/build/*/user/iproc-4_4/socdiag.o "$BCMUSER" "$BCMUSER.dbg" 2>/dev/null || true

echo "== building bcmd (docker $IMG) =="
# IMPORTANT: the cached libs were built with the SDK mounted at /sdk and ONL
# kernel at /kern (paths are baked into the .P dependency + generated files), so
# we MUST mount at the same points or make tries to regenerate libs and fails.
docker run --rm -u root:0 \
  -v "$SDK":/sdk -v "$KERNDIR":/kern:ro -w /sdk/systems/linux/user/iproc-4_4 \
  "$IMG" bash -lc '
    set -e
    KINC=$(arm-linux-gnueabihf-gcc -print-file-name=include)
    # INCLUDE_KNET (diag KNETctrl + KCOM glue) + BUILD_KNET=1 (link libuser KCOM);
    # the modules include dir provides uk-proxy-kcom.h. -Wno-error/-Wno-cpp: newer
    # glibc _BSD_SOURCE #warning under the SDKs -Werror.
    ADD_TO_CFLAGS="-Wno-error -Wno-cpp -DINCLUDE_KNET -I/sdk/systems/linux/kernel/modules/include" \
    make SDK=/sdk CROSS_COMPILE=arm-linux-gnueabihf- KERNDIR=/kern \
         TOOLCHAIN_BASE_DIR=/usr KFLAG_INCLD="$KINC" LINUX_MAKE_USER=1 \
         BUILD_KNET=1 MAKE=make -j6 bcm 2>&1
  ' 2>&1 | tail -40

[ -f "$BCMUSER" ] || { echo "[bcmd] build did not produce $BCMUSER" >&2; exit 1; }

mkdir -p "$ART"
cp -f "$BCMUSER" "$HERE/bcmd"
docker run --rm -u root:0 -v "$EDGE":"$EDGE" "$IMG" \
  arm-linux-gnueabihf-strip "$HERE/bcmd" 2>/dev/null || true
cp -f "$HERE/bcmd" "$ART/bcmd"

echo
echo "== result =="
file "$HERE/bcmd"
echo "staged: $HERE/bcmd  +  $ART/bcmd"
echo "deploy: scp bcmd root@10.1.1.209:/mnt/onl/data/  (run with config.bcm in CWD,"
echo "        BDE + linux-bcm-knet loaded, /dev/linux-bcm-knet present)"
