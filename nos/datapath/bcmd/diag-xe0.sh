#!/bin/sh
# diag-xe0.sh — SDK-reference A/B for the SFP+ xe0 forwarding blocker. Brings xe0
# up the STANDARD SDK way (port enable + HW linkscan, which should invoke the
# 84758 PHY driver datapath setup that bcmd's custom bring-up may bypass), then
# dumps the Warpcore DSC (compare vs ICOS icos_linked_2026_06_06/04_dsc_locked.txt),
# the 84758 line/system lock, and the MAC counters before/after a far-side ping
# window. bcm.user IS the SDK that ICOS's FASTPATH sits on -> if this forwards,
# bcmd's bring-up is the gap (no reflash needed).
set -e
D=/mnt/onl/data; cd "$D"
echo "[diag] stopping bcmd"; pkill -9 -x bcmd 2>/dev/null || true; sleep 1

[ -e /sys/bus/platform/drivers/iproc_cmic/48000000.iproc_cmicd ] && \
  echo 48000000.iproc_cmicd > /sys/bus/platform/drivers/iproc_cmic/unbind 2>/dev/null || true
lsmod | grep -q linux_kernel_bde || insmod linux-kernel-bde.ko dmasize=8M
lsmod | grep -q linux_user_bde   || insmod linux-user-bde.ko
lsmod | grep -q linux_bcm_knet   || insmod linux-bcm-knet.ko
for n in linux-kernel-bde linux-user-bde linux-bcm-knet; do
  maj=$(awk -v x="$n" '$2==x{print $1}' /proc/devices); [ -n "$maj" ] && { rm -f /dev/$n; mknod /dev/$n c "$maj" 0; }
done

echo "[diag] deasserting CPLD ext-PHY reset"
i2cset -f -y 0 0x30 0x07 0x02; i2cset -f -y 0 0x30 0x08 0x02; i2cset -f -y 0 0x30 0x0d 0x01
i2cset -f -y 0 0x30 0x19 0x00; i2cset -f -y 0 0x30 0x1b 0x00

echo "[diag] writing ICOS MIIM bus-map"
for pr in 004:0x00010012 010:0x08210001 058:0x00F00000 060:0xFFFFFFFE 064:0x1101FFFF \
          074:0x09249000 078:0x09249249 07c:0x03249249 080:0x00092480 084:0x00000002 \
          094:0x04030201 098:0x08070605 09c:0x0E0D0C0B 0a0:0x1211100F 0a4:0x18171615 \
          0a8:0x1C1B1A19 0ac:0x04030201 0b0:0x08070605 0b4:0x0E0D0C0B 0b8:0x1211100F 0bc:0x18171615; do
  o=${pr%%:*}; v=${pr##*:}; ./devmem $(printf "0x%x" $((0x48011000+0x$o))) $v >/dev/null
done

echo "[diag] launching bcm.user — SDK-standard xe0 bring-up + DSC/lock/counter dump"
./bcm.user <<'CMDS' 2>&1
init all
init bcm
linkscan on
sleep 3
port xe0 enable=1
sleep 8
echo ===PS_XE0===
ps xe0
echo ===DSC_XE0  (compare vs ICOS 04_dsc_locked)===
phy diag xe0 dsc
echo ===84758_LINE_SD_1.000a / BLOCKLOCK_3.0020===
phy xe0 0x1000a
phy xe0 0x30020
echo ===COUNTERS_BEFORE===
show c xe0
echo ===PINGWINDOW_25s  (far side: ping 10.101.102.2 now)===
sleep 25
echo ===COUNTERS_AFTER===
show c xe0
quit
CMDS
echo "[diag] done — restart datapath with: sh bringup-bcmd.sh (or relaunch bcmd)"
