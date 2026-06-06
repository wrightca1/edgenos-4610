# AS4610 SFP+ 10G — the bring-up orchestration (from the full XGS SDK) and how to finish it

The SFP+ 10G optical link is the last open item. We proved it's NOT a static gap:
the 84758 ucode is identical+running, all 384 PHY config regs match the locked SDK
state, and the per-PHY config matches. The gap is the **port-level bring-up
ORCHESTRATION** (order + a dynamic re-arm), which lives in the SDK's esw/port/phy
layer — not in `phy84740.c` alone.

## Source of truth
The complete reference is the **open-source XGS-ROBO 6.4.1 SDK** (Broadcom
source-available, same license family as OpenMDK/robo2):
- Official: `github.com/Broadcom-Network-Switching-Software/OpenBCM`
- Mirror w/ Helix4 (right era for BCM56340): `github.com/ariavie/bcm` → `sdk-xgs-robo-6.4.1/`
It has our exact chip + all the pieces, coordinated:
`src/soc/esw/helix4.c` (BCM56340), `src/bcm/esw/port.c` + `link.c` (port/link
orchestration), `src/soc/phy/wc40.c` (Warpcore), `src/soc/phy/phy84740.c` (84758).
Used as **reference** to implement in our BSD-3 `edged` (not redistributed) — same as
how we used `phy84740.c`.

## The cascaded-PHY model
Each SFP+ port = two stacked PHY drivers glued by `soc_phyctrl_*`:
- **ext** = BCM84758 (phy84740.c, Quadra/quad-port, 1 logical 10G port/lane)
- **int** = Warpcore WC40 serdes (wc40.c)
Helix4 has NO chip-specific port-up code (`helix4.c:3500` only registers the wc40 fw
helper; it reuses the Triumph3 path). All orchestration is in phyctrl.c + phy84740.c +
wc40.c.

## The exact port-up sequence (what makes the 84758 media RX acquire)
1. `phy_84740_init` configures the 84758 optical regs (RXLOS/MOD_ABS override c8e4,
   sig-level c800, etc. — phy84740.c:1552-1621) but then **disables TX**
   (`phy_84740_enable_set(FALSE)`, :1486) and **enables squelch**
   (`_phy_84740_squelch_enable(...,TRUE)`, :1640). Line side starts OFF.
2. `soc_phyctrl_speed_set` → `phy_84740_speed_set` (:5035): set ext PMA type 10G-LRM
   (c804 CTRL2, :5096), **then** `PHY_SPEED_SET(int_pc)` → Warpcore
   `_phy_wc40_ind_speed_set` (wc40.c:4280):
   - select firmware/EDC mode (UC_INFO_B1_FIRMWARE_MODE, :4331-4347), force 10G XFI/SFI
     + IND_40BITIF (:4400)
   - **ASSERT** `DIGITAL5_MISC6.RESET_RX_ASIC|RESET_TX_ASIC` (wc40.c:4366-4369)
   - program VCO/force-speed/TX-driver while held in reset
   - **DEASSERT** the reset (wc40.c:4456-4460)  ← serdes RX datapath re-acquires here
3. `soc_phyctrl_enable_set(TRUE)` (phyctrl.c:1307), strict order:
   - **(a)** Warpcore enable FIRST (phyctrl.c:1328) → `_phy_wc40_enable_set` (wc40.c:3861):
     clear XGXSBLK1_LANECTRL3 PWRDN_RX|PWRDN_TX|PWRDWN_FORCE (:3886), clear
     XGXSBLK0_MISCCONTROL1.GLOBAL_PMD_TX_DISABLE (:3892), clear CL73 sigdet timer-dis.
   - **(b)** 84758 enable SECOND (phyctrl.c:1331) → `phy_84740_enable_set(TRUE)`
     (phy84740.c:2587): clear global PMD TX-disable (:2615), per-lane clear OPTICAL_CFG
     c8e4 TxOn(b12)+power(b4) (:2634-2669), c800[7] toggle if c8e4[4]=1 (:2652).
   - repeater/Quadra only: un-squelch line RX by clearing 0xcd18 squelch bits
     (`_phy_84740_squelch_enable`, :975-991), gated on (0xc81d & 0x6)==0x6.
4. (optional, if forcing a serdes DSC/fw mode) Warpcore uC handshake on DSC1B0_UC_CTRL
   0x820e: STOP→set-mode→RESUME→RESTART, poll READY_FOR_CMD bit7 w/ 1ms delays
   (wc40.c:672, 724-808). This re-arms the RX EQ.

Net: serdes is **reset-cycled and established FIRST**, THEN the 84758 line side is
re-armed against it. Order: serdes-speed(+reset) → serdes-enable → 84758-enable/un-squelch.

## What edged is missing (implement these, in this order)
edged already: loads/runs 84758 ucode, matches all phy84740 optical config, and a
disable→config→enable cycle got the **Warpcore system side** to RX-lock. Missing:
1. **Warpcore RX/TX-ASIC reset CYCLE during speed-set** — OpenMDK's `bcmi_warpcore_xgxs`
   speed_set does clock-comp + FIRMWARE_MODE but (per earlier analysis) no
   DIGITAL5_MISC6 RX/TX-ASIC assert→deassert. Add it: via phy_aer_iblk write the
   Warpcore DIGITAL5_MISC6 reset bits (get exact addr/bits from wc40.c:4366/4456),
   assert → (speed already set) → deassert, after configuring the lane.
2. **Strict order**: 84758 PMA-type(10G-LRM) → Warpcore speed-set(+reset cycle) →
   Warpcore enable (un-powerdown) → 84758 TX-enable/un-squelch. edged currently enables
   the 84758 (sfp_tx_enable) without guaranteeing the serdes datapath was reset-cycled
   and enabled first.
3. **Deferred 84758 re-enable AFTER the serdes is up** (the sfp_tx_enable TX/un-squelch
   must run strictly after the Warpcore reset-deassert + enable, not before).
Box is iterable on edged for all of this (no reflash needed for PHY work).

## File:line index (sdk-xgs-robo-6.4.1)
- bcm/esw/port.c: 2900 port_enable_set; 2983/2991 phy-then-mac; 7379 _bcm_port_update; 7412 linkup_evt
- bcm/esw/link.c: 2182 update on up-edge
- soc/common/phyctrl.c: 802/896/907 init order; 1198/1238 link_get(ext); 1307/1328/1331 enable order
- soc/phy/phy84740.c: 1486 init-disables-TX; 1552-1621 optical cfg; 1640 init squelch(TRUE); 2587 enable_set; 975-991 squelch; 5035/5096/5126 speed_set
- soc/phy/wc40.c: 3861 enable(power-up); 4280 ind_speed_set; 4366-4369/4456-4460 RX/TX-ASIC reset cycle; 672/724-808 0x820e fw handshake
- soc/esw/helix4.c: 3500 (wc40 fw helper only; uses TR3 port path)

## CONFIRMED SPECIFIC GAP (2026-06-06) — the Warpcore 0x820e fw-pause handshake
Read OpenMDK's `bcmi_warpcore_xgxs_drv.c`: `_warpcore_serdes_stop` (:386) only
power-downs TX via LANECTRL3, and the firmware-mode set (:1255+) writes FIRMWARE_MODE
directly. **Neither does the UC_CTRL (0x820e) STOP -> set-mode -> RESUME -> RESTART
handshake** (poll READY_FOR_CMD bit7, 1ms delays) that the SDK's
`_phy_wc40_firmware_mode_set` does (wc40.c:672, 724-808). The SDK trace explicitly
noted skipping this handshake yields OUR EXACT symptom: "system side locking but the
media side never adapting" = Warpcore RXlink=1 (we have this) + 84758 media sd=0.
This is ALSO the same handshake decoded on the AS5610 (project_wc_fw_pause_protocol /
docs/WC_FIRMWARE_PAUSE_PROTOCOL.md).

### NEXT ACTION (implement in edged, test live — no reflash):
Implement the Warpcore 0x820e UC_CTRL fw-pause/host-cal handshake around the XFI
firmware-mode set: STOP (write UC_CTRL stop, poll READY_FOR_CMD) -> set FIRMWARE_MODE
for the lane -> RESUME -> RESTART, re-arming the RX DSC/EQ. Two reference impls to
follow: SDK wc40.c:672+724-808, and the 5610 decode in docs/WC_FIRMWARE_PAUSE_PROTOCOL.md.
Apply per-SFP+-lane after the Warpcore speed-set, then re-check 84758 media sd / PCS
block-lock. This is the leading (doubly-referenced) candidate to finally lock the link.
