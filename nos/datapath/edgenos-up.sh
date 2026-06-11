#!/bin/sh
# edgenos-up.sh — one-shot full datapath + control-plane bring-up driven by the
# config files, for STOCK ONL where /etc and /usr reset every boot so the proper
# systemd units (rootfs-overlay/etc/systemd/system/*) can't persist. Everything
# lives in /mnt/onl/data/edgenos (the persistent partition). This is the
# stop-gap until the config-file scheme is baked into a custom EdgeNOS-4610 SWI
# (rootfs-overlay/ mirrors exactly what that image would ship).
#
# Run on the box:  sh /mnt/onl/data/edgenos/edgenos-up.sh
# Idempotent: re-running re-applies config without duplicating.
set -e
HERE=/mnt/onl/data/edgenos
export EDGENOS_ETC="$HERE/etc/edged"

echo "[edgenos-up] platform prep + bcmd"
sh "$HERE/usr/sbin/bcmd-prep.sh"
pkill -x bcmd 2>/dev/null || true; sleep 1
( cd /mnt/onl/data && setsid ./bcmd >/tmp/bcmd.log 2>&1 </dev/null & )

echo "[edgenos-up] waiting for bcmd datapath UP"
i=0; while ! grep -q "datapath UP" /tmp/bcmd.log 2>/dev/null; do
    i=$((i+1)); [ "$i" -gt 60 ] && break; sleep 2; done

echo "[edgenos-up] applying L3 config (addrs + routes)"
sh "$HERE/usr/sbin/edgenos-l3-config.sh"

echo "[edgenos-up] starting Quagga zebra + ospfd"
mkdir -p /var/run/quagga
pkill -f "$HERE/sbin/zebra-arm" 2>/dev/null || true
pkill -f "$HERE/sbin/ospfd-arm" 2>/dev/null || true
sleep 1
( cd /tmp && setsid "$HERE/sbin/zebra-arm" -f "$HERE/etc/quagga/zebra.conf" </dev/null >/tmp/zebra.log 2>&1 & )
sleep 3
( cd /tmp && setsid "$HERE/sbin/ospfd-arm" -f "$HERE/etc/quagga/ospfd.conf" </dev/null >/tmp/ospfd.log 2>&1 & )
sleep 2
echo "[edgenos-up] done — bcmd=$(pgrep -x bcmd||echo -) zebra=$(pgrep -x zebra-arm||echo -) ospfd=$(pgrep -x ospfd-arm||echo -)"
