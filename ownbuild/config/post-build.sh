#!/bin/sh
# EdgeNOS-4610 buildroot post-build: runs against $TARGET_DIR (the rootfs staging tree).
set -e
TGT="$1"
# permit root SSH (switch console use)
if [ -f "$TGT/etc/ssh/sshd_config" ]; then
    sed -i 's/^#*PermitRootLogin.*/PermitRootLogin yes/' "$TGT/etc/ssh/sshd_config"
    grep -q '^PermitRootLogin yes' "$TGT/etc/ssh/sshd_config" || echo 'PermitRootLogin yes' >> "$TGT/etc/ssh/sshd_config"
fi
# datapath scripts/binaries executable
chmod +x "$TGT"/opt/edgenos/*.sh "$TGT"/opt/edgenos/bcmd "$TGT"/opt/edgenos/*-arm 2>/dev/null || true
# ONL-compat persistent mountpoints (harmless if unused)
mkdir -p "$TGT/mnt/onl/data" "$TGT/mnt/onl/boot" "$TGT/mnt/onl/images"
