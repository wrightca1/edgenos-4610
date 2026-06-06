#!/bin/sh
# apply.sh — apply the EdgeNOS-4610 OpenMDK patches to a clone of OpenMDK.
# Usage: ./apply.sh /path/to/OpenMDK
set -e
MDK="${1:?usage: apply.sh /path/to/OpenMDK}"
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -d "$MDK/bmd/PKG" ] || { echo "not an OpenMDK tree: $MDK" >&2; exit 1; }

cd "$MDK"
for p in "$HERE"/0*.patch; do
    echo "applying $(basename "$p")"
    patch -p1 < "$p"
done

# Install the BCM84758 ucode (Broadcom source-available, shipped in this repo) into
# both the canonical PKG/ source and the generated pkgsrc/ build tree.
for d in phy/PKG/chip/bcm84740 phy/pkgsrc/chip/bcm84740; do
    if [ -d "$MDK/$d" ]; then
        cp "$HERE/bcm84758_ucode.c" "$MDK/$d/"
        echo "installed bcm84758_ucode.c -> $d"
    fi
done

echo
echo "Done. Rebuild with build-datapath.sh."
