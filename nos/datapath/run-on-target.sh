#!/bin/sh
# run-on-target.sh — first contact with the BCM56340 CMIC on the AS4610-54T.
# Copy this + ./linux-user-mdk to the switch (e.g. /tmp) and run it there.
#
# Path A access: maps the on-die CMIC at physical 0x48000000 via /dev/mem
# (STRICT_DEVMEM is off in the ONL kernel) — no Broadcom kernel module needed.
# See BDE_ACCESS.md.
#
# Drops you into the CDK/BMD shell. First thing to try (sanity): a register
# read, e.g.  `get CMICM_REVIDr`  (memmapped CMIC reg), should return a sane
# value — proving we're really on the CMIC.
#
# CHIP BRING-UP ORDER MATTERS:  reset  ->  init  ->  swinit
#   `reset`  sets up TOP_CORE_PLL + core clocks (REQUIRED first; without it the
#            core is unclocked and every SCHAN op fails: "S-channel error ...
#            IPIPE reset timeout").
#   `init`   block init (ING/EGR/ISM/AXP resets) over SCHAN.
#   `swinit` L2 switching init.
# Then `portmode`, `pinfo <ports>`, `vlan`, `tx`/`rx`, ...
# Verify SCHAN after reset+init:  get TOP_DEV_REV_IDr  -> should read 0x0001b340.
set -e
BIN="${BIN:-./linux-user-mdk}"
UNIT="0x14e4:0xb340:0x01@0x48000000"   # vendor:device:rev @ CMIC phys base

[ -r /dev/mem ] || { echo "ERROR: /dev/mem not readable (need root)"; exit 1; }
[ -x "$BIN" ]   || { echo "ERROR: $BIN not found/executable"; exit 1; }

echo "Attaching BCM56340 (Helix4) via /dev/mem @ 0x48000000 ..."
exec "$BIN" --unit "$UNIT"
