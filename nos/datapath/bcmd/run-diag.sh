#!/bin/sh
# run-diag.sh — drive the stock SDK diag shell to investigate the SFP+ (xe0/port49)
# RX-lock. Deasserts the CPLD ext-PHY reset + writes the ICOS MIIM bus-map FIRST
# (so init all attaches the awake 84758), then feeds diagnostic commands.
set -e
D=/mnt/onl/data; cd "$D"
echo "[diag] stopping bcmd"; pkill -9 -x bcmd 2>/dev/null || true; sleep 1

# modules (idempotent)
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
echo "[diag]   CPLD 0x19=$(i2cget -f -y 0 0x30 0x19) 0x1b=$(i2cget -f -y 0 0x30 0x1b)"

echo "[diag] writing ICOS MIIM bus-map"
for pr in 004:0x00010012 010:0x08210001 058:0x00F00000 060:0xFFFFFFFE 064:0x1101FFFF \
          074:0x09249000 078:0x09249249 07c:0x03249249 080:0x00092480 084:0x00000002 \
          094:0x04030201 098:0x08070605 09c:0x0E0D0C0B 0a0:0x1211100F 0a4:0x18171615 \
          0a8:0x1C1B1A19 0ac:0x04030201 0b0:0x08070605 0b4:0x0E0D0C0B 0b8:0x1211100F 0bc:0x18171615; do
  o=${pr%%:*}; v=${pr##*:}; ./devmem $(printf "0x%x" $((0x48011000+0x$o))) $v >/dev/null
done

echo "[diag] launching bcm.user with command script"
./bcm.user <<'CMDS' 2>&1
init all
init bcm
linkscan on
sleep 6
echo ===PORTS===
ps ge
quit
CMDS
echo "[diag] done"
