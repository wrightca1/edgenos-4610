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

echo
echo "Patches applied. NOTE: also install the BCM84758 ucode (kept local, see"
echo "README.md 'Firmware') into phy/PKG/chip/bcm84740/ and phy/pkgsrc/chip/bcm84740/"
echo "then rebuild with build-datapath.sh."
