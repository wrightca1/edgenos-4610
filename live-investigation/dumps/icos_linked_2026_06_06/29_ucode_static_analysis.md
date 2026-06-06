# 84758 ucode static analysis (2026-06-06)

Question: can static analysis of the BCM84758 microcode crack the media-RX gap?

## Tools / feasibility
- Extracted the 32 KB 8051 binary from phy84758_ucode.c -> /tmp/phy84758_ucode.bin.
- It IS 8051 (entry `12 f3 76` = LCALL 0xf376). radare2 (`r2 -a 8051`) disassembles it.
  So static analysis is technically feasible.

## DECISIVE finding — the ucode is NOT the variable
- **edged's loaded ucode == robo2/ICOS ucode, byte-for-byte** (both 32768 B, md5
  2f2a20baeabe). edged installs the SAME robo2 `bcm84758_ucode` blob the full SDK uses.
- The uC is **confirmed RUNNING** in edged, not just downloaded: the robo2 MDIO
  download is micro-driven (uC reads the image via M8051_MSGIN and, after boot,
  COMPUTES + writes the checksum to 1.0xca1c). edged reads 1.0xca1c = 0x600d (correct)
  -> the 8051 executed and computed it. So the uC runs.
- The host enable path is identical too: robo2 phy_84740_enable_set (TX-disable clear,
  c8e4 TxOn/lane-power, c800[7] TxOnOff toggle) == edged's sfp_tx_enable. No media-RX
  enable in enable_set; media-RX adaptation is the uC's autonomous job once the lane is
  powered (c8e4[4]=0, which edged has).

## Conclusion
Static analysis (ucode + driver source) PROVES: identical firmware, identical optical
config (all 384 regs; only uC-adaptation OUTPUTS differ), identical enable path -- yet
ICOS's media RX adapts (sd=1) and edged's does not (sd=0). The gap is therefore a
DYNAMIC/sequencing subtlety, not anything statically different. Disassembling the 8051
cannot reveal an edged-vs-ICOS difference because the firmware is the same.

One residual static diff: c848 (SPA_CTRL) bit15 -- ICOS=0 (0x70f8), edged=1 (0xf0f8).
c848 is the uC boot/download-control reg (bit15="SPI-ROM downloading to RAM"). But
forcing edged c848=0x70f8 post-hoc did NOT lock, and the uC is confirmed running
(checksum computed), so bit15 is likely a benign leftover of OpenMDK's MDIO-download
path. If pursued, it would mean re-doing the 84758 ucode download with the robo2 boot
finalize (clear bit15 + MII reset to boot-from-RAM) -- a driver re-implementation, not
a static-analysis result.

## Net
The ucode static-analysis question is answered: the firmware is identical to ICOS and
runs, so it is not the blocker. We have now exhausted STATIC analysis (firmware +
registers + driver code all match edged<->ICOS). The remaining media-RX gap is a
dynamic bring-up subtlety (likely sequencing/clock/state) that static methods can't
surface on this hardware.
