# BCM84758 SFP+ PHY — integration into the OpenMDK datapath

Goal: drive the AS4610-54T's 10G SFP+ PHY (BCM84758, xe0-3) from `edged` so its
firmware loads and the ports link. The 84758 is **84740-family**, so we extend
OpenMDK's existing `bcm84740` driver rather than port the differently-structured
robo2-xsdk driver.

## Changes made (2026-06-05)

### 1. New ucode file (verified Broadcom source-available firmware)
`OpenMDK/phy/pkgsrc/chip/bcm84740/bcm84758_ucode.c` — generated from
`phy84758-src/broadcom-official/phy84758_ucode.c` (robo2-xsdk; byte-identical to
the copy in our own ICOS, sha256 64ae5619…). Symbols renamed to OpenMDK
convention: `bcm84758_ucode[]` / `bcm84758_ucode_len` (32768 B, v0128).
Auto-compiled (the chip-dir Makefile globs `*.c`).

### 2. `OpenMDK/phy/pkgsrc/chip/bcm84740/bcm84740_drv.c` (3 edits)
- `extern bcm84758_ucode[]` / `_len`.
- `#define BCM84758_PMA_PMD_ID0 0x600d`, `ID1 0x86f0`, `CHIP_ID 0x84758`
  (live 84758 PMA-PMD ID, vs 84740's 0x0362:0x5fd0).
- `bcm84740_phy_probe`: also claim the 84758 ID (shared driver flow).
- `_bcm84740_init_stage_3`: re-read PMA-PMD ID and download `bcm84758_ucode`
  for the 84758, else `bcm84740_ucode`. (Post-download success marker is
  `GEN_REG_4 == 0x600d` for the WHOLE family incl. 84758 — robo2-xsdk
  `phy84740.c:1065` "1.ca1c==600d" — so the existing check is correct.)

### 3. `nos/datapath/build-datapath.sh`
Added `-DPHY_CONFIG_INCLUDE_BCM84740=1` so the driver is registered in the probe
list (was off in the default 56340 phy config).

## Status: code IN + builds + edged runs — but 84758 NOT yet driven
`edged` (2.17 MB) links the 84758 ucode + extended driver and runs fine. BUT a
verbose init shows only the **internal Warpcore** PHYs initializing — the
external 84758 is **never probed**. Root cause: OpenMDK's bcm56340 BMD computes
external-PHY MDIO addresses via `_phy_addr_get()` (bcm56340_a0_bmd_reset.c) which
does NOT yield the AS4610's `0x40-0x43`, and `bcm56340_a0_bmd_attach.c` registers
no external PHY ctrl for the SFP+ ports. So the driver/ucode are ready but the BMD
doesn't reach the PHY.

## NEXT: wire the external PHY address map (no module needed — PHY is on-board)
Make the BMD attach an external PHY on xe0-3 at MDIO `0x40+N` (per live
`config.bcm.as4610-54t`): either patch `_phy_addr_get` for the AS4610 SFP+ ports
or register the external PHY ctrl in bmd_attach with the right addr. Then probe
binds → ucode downloads (verify: PHY reg `1.0xca1c == 0x600d`). Validate link with
a 10G module + partner.

## License note
`bcm84758_ucode.c` carries the robo2-xsdk header (Broadcom source-available, same
grant as OpenMDK; preserve the notice). [[../../../LICENSING.md]]

## UPDATE 2026-06-05 — external addr patched, but 84758 still not probed
Patched `bcm956340k_miim_ext.c` `_phy_addr()`: ports 53-56 -> (port-53)+EBUS(1) =
0x40-0x43 (EBUS(_b)=_b<<6). Rebuilt; edged runs. BUT only the internal Warpcore
still binds to ports 53/57/61 — the external 84758 is not found. The BMD probe
(bmd_phy_probe_default.c) DOES cascade (loops all buses, one PHY per bus), so it
should try the external bus after the Warpcore. Cause is narrowed to the
address/port mapping: (a) external bus not assigned to 53-56, (b) SFP+ ports not
53-56, or (c) 84758 doesn't answer at EBUS(1)/0x40-0x43.
NEXT: empirical MDIO scan (cdk_xgsm_miim_read over EBUS(0..3) x addr 0..7, read
PMA-PMD ID devad1 reg2/3) to find where 0x600d:0x86f0 responds -> fix _phy_addr ->
probe binds -> ucode downloads (verify PHY 1.0xca1c==0x600d).

## UPDATE 2026-06-05 #2 — MDIO scan: 84758 not reachable on external bus
Added `edged --scan-mdio` (cdk_xgsm_miim_read over EBUS0-3 + IBUS0-3, addr 0-15,
PMA-PMD devad1 reg2/3). Result on live box:
  IBUS2 addr 0x01/0x05/0x09 = ID 0143:bff0  (the 3 internal Warpcores)
  -> NO 0x600d:0x86f0, NO external-bus PHY anywhere (incl. 0x40, 0xc1).
So the external 84758 does not answer over OpenMDK's CMIC MIIM as configured —
deeper than the addr map: the **external MIIM master/clock/mux isn't set up** by
the generic bcm56340 BMD for this board (internal MDIO works; external doesn't).
ICOS reaches it at addr 0x40 (iaddr 0xc1) — so the path exists, just needs the
CMIC external-MDIO enable/clock-divider config.
NEXT options: (1) compare ICOS/full-SDK CMIC MIIM external-bus setup (CMICM MIIM
clock/enable regs) and replicate; (2) given the SFP+ path keeps surfacing board-
specific layers, prioritize the QSFP-as-10G path (internal Warpcore, already
probes/inits — no external PHY) for first 10G, and revisit SFP+ with a module.

## BREAKTHROUGH 2026-06-05 #3 — 84758 located + reached on iProc CCB MDIO
External PHYs are NOT on the CMIC SBUS MIIM — they're on the iProc CCB MDIO master
(brcm,iproc-ccb-mdio @ 0x18032000; enable reg 0x1803fc3c bit4 = DT iproc-mdio-sel-bit;
MGMT_CTL@+0 [PRE=b7,BSY=b8,EXT=b9,MDCDIV=b0-6], MGMT_CMD_DATA@+4 [SB=30,OP=28,PA=23,
RA=18,TA=16,DATA=0-15]). Implemented iProc MDIO read in edged (mmap /dev/mem + the
protocol from Linux mdio-xgs-iproc-cc.c). `edged --scan-mdio` now also scans it.
RESULT: MGMT_CTL=0x9a, enable=0x1c (bit4 set), and **C22 phy addr 0x01 = ID 0x0362:5d12**
= 84740-family (OpenMDK BCM84740_PMA_PMD_ID0=0x0362) = THE 84758, reachable!

REMAINING:
1. C45 read framing returned nothing (used SB=0/OP=0addr/OP=3read) — fix it (the
   84758 PMA-PMD regs are C45). Bus works (C22 proved it), so it's the C45 frame.
2. Correct the driver's BCM84758 probe ID: it presents 0x0362:0x5d1x here, not the
   0x600d:0x86f0 I took from the ICOS `phy info` column (different reg space). Re-read
   the real C45 PMA-PMD ID once C45 works, then set bcm84740_drv.c match accordingly.
3. Wire the iProc CCB MDIO as a phy_bus for the SFP+ ports (replace/augment the CMIC
   ext bus) so the probe binds the 84758 there -> init_stage_3 downloads the ucode
   (verify PHY 1.0xca1c==0x600d). 84758 is at iProc-ext phy addr 1 (+ its quad siblings).

## 2026-06-05 #4 — full MDIO topology mapped; front PHYs behind a board mux
edged --scan-mdio now sweeps CMIC MIIM + iProc CCB MDIO (EXT=1 and EXT=0):
  CMIC SBUS MIIM @0x48000000        -> internal Warpcore SerDes only (0143:bff0, IBUS2)
  iProc CCB MDIO @0x18032000 EXT=0  -> internal Warpcore SerDes (0143:bff0, addr 1-5,1f)
  iProc CCB MDIO @0x18032000 EXT=1  -> ONLY the mgmt-port PHY (0x0362:5d12, addr1, linked)
NONE reach the front-panel 0x600d PHYs (54282 1G / 84758 10G). ICOS reaches them via CMIC
MIIM addrs 0x01-0x3c (ge) / 0x40-0x43 (xe) (iaddr 0x81=EBUS2, 0xc1=EBUS3). => there is a
BOARD-LEVEL MDIO MUX (CPLD/GPIO) selecting the external MDIO segment; default position after
our reset/init exposes the mgmt segment, not the front-panel segments. Not in DTS, not in our
partial CPLD map. C45 framing on the iProc master also returns 0xffff (mgmt PHY is C22-only, so
that's expected there; can't confirm C45 until we reach a real C45 PHY).

NEXT (to crack the mux — pick one):
  (a) RE the ICOS switchdrvr (backup/icos-extract/switchdrvr): find the CPLD/GPIO writes it does
      before MDIO-reading the front PHYs (the mux-select sequence). Definitive but hard (stripped).
  (b) Systematic CPLD probe: dump CPLD 0x30 fully, toggle candidate mux/select regs, re-scan MDIO
      for 0x600d. Writes to live board (our box) — bounded risk.
  (c) Bank QSFP-as-10G (internal Warpcore, already probes/inits, NO external MDIO) as the testable
      10G path; revisit SFP+ when a module is available to validate link anyway.
This is the 4610's analogue of the 5610 multi-week PHY battle: external-PHY MDIO routing.

## ✅✅✅ 2026-06-05 #5 — DONE: integrated into edged, ucode loads from a clean boot
The full external-MDIO recipe is now wired into `edged`'s init flow and verified on
the live box (ma1=10.1.1.209):
  main(): _create_cdk_device
    -> deassert_ext_phy_reset()   (CPLD 0x07=2,0x08=2,0x0d=1,0x19=0,0x1b=0)
    -> write_icos_miim_map()      (22 regs @0x48011000, bus map)
    -> bmd_phy_probe_init / attach / reset / init
    -> write_icos_miim_map()      (re-assert after init)
    -> swinit / ports / STP / inventory
    -> 84758 fw-checksum verify: cdk_xgsm_miim_read(unit,0x80,(1<<16)|0xca1c) == 0x600d
Run output: `84758(xe0) PMA-PMD ID1=0x86f0 fw-checksum(1.0xca1c)=0x600d <== UCODE LOADED`.

### CPLD-write reliability fix (the last blocker)
The first integrated run logged `cpld: write 0x0d/0x19/0x1b failed` — those writes only
"worked" because of residual manual `i2cset` state. ROOT CAUSE: the ONL
`accton_as4610_cpld` kernel driver is bound to i2c-0 0x30 and **actively writes 0x1b
itself** (observed: it rewrites 0x1b=0x19 right after edged exits). edged's old raw
`write(fd,{reg,val},2)` raced that driver and silently dropped transactions.
FIX (edged.c `cpld_write`): use **SMBus byte-data** ioctls (`I2C_SMBUS` /
`I2C_SMBUS_WRITE` / `I2C_SMBUS_BYTE_DATA`) — the same transaction type the kernel
driver uses — with an **8x retry loop + 3 ms inter-try delay + read-back verify**
(accept if read-back matches or the reg is unreadable/strobe). `deassert_ext_phy_reset`
now reads back 0x19/0x1b and logs `0x19=0x00 0x1b=0x00 verified`.
Validated from the held-in-reset power-on default (0x19=0x7f, 0x1b=0xb5): edged clears
both to 0x00 with NO "failed" lines and the ucode loads — across repeated runs. The
SMBus union/constants are pulled from `<linux/i2c.h>` (with an inline ABI fallback for
toolchains that lack it).

### Known follow-up (not blocking ucode load)
After edged exits, the bound CPLD driver rewrites 0x1b=0x19 on its own. The ucode is
already downloaded and the PHY is out of reset by then, so link is unaffected for a
one-shot init. But for a persistent/SWI-baked deployment, watch whether the driver's
later 0x1b touches re-assert reset on any xe lane over time; if so, either keep edged
holding the reg or unbind the ONL CPLD driver from the reset regs. Still needs a 10G
module + link partner to confirm actual SFP+ link training.
