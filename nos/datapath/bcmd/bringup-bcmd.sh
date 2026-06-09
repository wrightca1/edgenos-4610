#!/bin/sh
# bringup-bcmd.sh — full from-scratch bring-up of the EdgeNOS-4610 datapath on a
# fresh boot. Deploy this + the 3 .ko modules + bcmd + config.bcm to
# /mnt/onl/data (persistent), then run this script as root on the box.
#
#   1. unbind the BCM56340 CMIC from the built-in iproc_cmic driver
#   2. insmod our GPL BDE (kernel + user) + KNET, with an 8MB DMA pool
#   3. create the char devnodes (majors are dynamic -> read /proc/devices)
#   4. launch bcmd (the BCM-API datapath daemon)
#
# Idempotent-ish: kills a running bcmd and reloads KNET for a clean RX ring.
set -e
D=/mnt/onl/data
cd "$D"

echo "[bringup] stopping any running bcmd"
pkill -9 -x bcmd 2>/dev/null || true; sleep 1

# --- 1. unbind CMIC from the built-in driver (ignore if already unbound) ---
if [ -e /sys/bus/platform/drivers/iproc_cmic/48000000.iproc_cmicd ]; then
    echo "[bringup] unbinding 48000000.iproc_cmicd from iproc_cmic"
    echo 48000000.iproc_cmicd > /sys/bus/platform/drivers/iproc_cmic/unbind 2>/dev/null || true
    sleep 1
fi

# --- 2. load modules (kernel-bde probes the 56340 + owns the DMA pool) ---
load() { mod="$1"; shift; lsmod | grep -q "^$(basename "$mod" .ko | tr - _)" || insmod "$mod" "$@"; }
echo "[bringup] loading BDE + KNET modules"
load linux-kernel-bde.ko dmasize=8M
load linux-user-bde.ko
load linux-bcm-knet.ko

# --- 3. devnodes (dynamic majors) ---
mknode() {
    name="$1"; maj=$(awk -v n="$name" '$2==n{print $1}' /proc/devices)
    [ -n "$maj" ] || { echo "[bringup] no major for $name"; return 1; }
    rm -f "/dev/$name"; mknod "/dev/$name" c "$maj" 0; chmod 644 "/dev/$name"
}
mknode linux-kernel-bde
mknode linux-user-bde
mknode linux-bcm-knet
echo "[bringup] devnodes:"; ls -l /dev/linux-* 2>&1

# --- 3b. deassert external-PHY CPLD reset (84758 SFP+ + 54282 copper) ---
# EdgeNOS/ONL boots with CPLD 0x19=0x7f/0x1b=0xef HOLDING the external PHYs in
# reset; the SDK can't clear a board CPLD GPIO, so MIIM to the 84758 returns
# 0xffff. ICOS clears them. Must run BEFORE bcmd's `init all` so the SDK's PHY
# probe finds + attaches the now-awake 84758. i2cset is reliable past the bound
# accton_as4610_cpld driver (-f force); the in-process attempt is best-effort.
echo "[bringup] deasserting external-PHY CPLD reset (i2c-0 0x30)"
i2cset -f -y 0 0x30 0x07 0x02 2>/dev/null || true
i2cset -f -y 0 0x30 0x08 0x02 2>/dev/null || true
i2cset -f -y 0 0x30 0x0d 0x01 2>/dev/null || true
i2cset -f -y 0 0x30 0x19 0x00 2>/dev/null || true
i2cset -f -y 0 0x30 0x1b 0x00 2>/dev/null || true
echo "[bringup]   CPLD 0x19=$(i2cget -f -y 0 0x30 0x19 2>/dev/null) 0x1b=$(i2cget -f -y 0 0x30 0x1b 2>/dev/null) (want 0x00 0x00)"

# --- 4. launch bcmd ---
echo "[bringup] launching bcmd"
setsid ./bcmd </dev/null >/tmp/bcmd.log 2>&1 &
echo "[bringup] bcmd pid $!  (log: /tmp/bcmd.log)"
echo "[bringup] done — give init ~25s, then check /tmp/bcmd.log and 'ip link show knet25'"
