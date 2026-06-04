#!/bin/sh
# run-on-target.sh — first contact with the BCM56340 CMIC on the AS4610-54T.
# Copy this + ./linux-user-mdk to the switch (e.g. /tmp) and run it there.
#
# Path A access: maps the on-die CMIC at physical 0x48000000 via /dev/mem
# (STRICT_DEVMEM is off in the ONL kernel) — no Broadcom kernel module needed.
# See BDE_ACCESS.md.
#
# Drops you into the CDK/BMD shell. First thing to try (sanity): a register
# read, e.g.  `get CMIC_DEV_REV_ID`  (or `getreg`/`help`), should return a sane
# value — proving we're really on the CMIC. Then `init`, `port`, `vlan`, ...
set -e
BIN="${BIN:-./linux-user-mdk}"
UNIT="0x14e4:0xb340:0x01@0x48000000"   # vendor:device:rev @ CMIC phys base

[ -r /dev/mem ] || { echo "ERROR: /dev/mem not readable (need root)"; exit 1; }
[ -x "$BIN" ]   || { echo "ERROR: $BIN not found/executable"; exit 1; }

echo "Attaching BCM56340 (Helix4) via /dev/mem @ 0x48000000 ..."
exec "$BIN" --unit "$UNIT"
