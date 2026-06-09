# ICOS working-ge25 copper-RX capture (2026-06-07)

Reflashed to ICOS 3.4.3.7 to capture the working copper-RX config on ge25 (front
port 1, the port edged leaves at rx_pkts=0). Cable connected throughout.

## HEADLINE — RX works on ICOS, same HW (proves software gap)
`show c ge25` on ICOS:  RPKT=6,230 (+1,514, ~19/s), RBYT=1.43MB (3,888 B/s),
RMCA=3,681 RBCA=1,648 RVLN=5,553 — heavy RX from the wire. TX tiny (TPKT=39).
=> Same board/cable/partner where edged shows rx_pkts=0. The copper-RX blocker is
100% EdgeNOS/OpenMDK software, NOT hardware.  ge25: up 1G FD SGMII Forward.

## THE actionable lead (config show)
**phy_sgmii_autoneg_ge=1** — ICOS enables SGMII autoneg on ALL GE ports. This drives
the SGMII config-word handshake between the chip QSGMII serdes (SGMII slave) and the
54282 system side (sends config word). edged/OpenMDK does NOT enable this; that is
almost certainly why the 54282->chip RX data path never forwards.
(Analogue of the phy_fiber_pref_xe lead that was key for the SFP+ side.)
Only GE-relevant phy property in the whole config — no other QSGMII/serdes tuning.

## QSGMII quad map (phy_port_primary_and_offset_geN)
ge24=0x1900 ge25=0x1901 ge26=0x1902 ge27=0x1903  => ge24-27 = one QSGMII quad,
primary=ge24, ge25 is subchannel offset 1. (Each 0xPPOO: PP=primary phys port, OO=offset.)
port_phy_addr_ge25=0x22 (EBUS a1). bcm56340_4x10=1, phy_sgmii_autoneg_ge=1, pbmp_xport_ge=0x3fffffffffffe.

## SGMII_SLAVE SERDES_LINK is a RED HERRING
ICOS SGMII_SLAVE (RDB0x235, reg0x1c bank0x15) reads ~0x5400 (data~0) => SERDES_LINK=0,
speed=0, dup=0 on the WORKING port too. So the SERDES_LINK=0 edged flagged is NOT the
differentiator. (Note: bcmsh two-step shadow reads are unreliable — returned only the
bank echo with 0 data even for MODE_CTRL on a linked port; do not trust shadow reads
captured via bcmsh.)

## phy diag ge25 dsc (serdes RX state, working)
Port 26: PLL Range 9, core temp ok. Lane 0 SD=1 (signal detect), lanes 1-3 SD=0
(QSGMII uses 1 physical lane). PF/VGA/DFE all 0 (clean, no DFE), TX_DRVR lane0=0x260,
MAIN/POSTC=0. => serdes lane 0 RX-locked; tuning is default/clean.

## ICOS phy ge25 standard MII (working) — for reference
00:1140 01:79ed 02:600d 03:845b 04:0141 05:c9e1 06:006d 07:2001
08:4162 09:0600 0a:7c00 0b:0000 0c:0000 0d:4007 0e:0000 0f:3000
10:0001 11:3f00 12:0000 13:0000 14:0101 15:871c 16:243e 17:0f7e
18:8800 19:374e 1a:0009 1b:0000 1c:001c 1d:0000 1e:0001 1f:2f00
(reg09 1000BT-ctrl=0x0600 adv-1000FD+repeater; edged had 0x0300. reg0a status 0x7c00.)

## NEXT (on EdgeNOS): implement phy_sgmii_autoneg
Replicate what the SDK does when phy_sgmii_autoneg_ge=1: enable SGMII autoneg
config-word handshake on BOTH ends — chip QSGMII serdes (AE|RAN as SGMII slave) AND
the 54282 system side (send config word / SGMII master). My earlier AN-only-on-serdes
attempt regressed because the 54282 side wasn't set to send the config word. Study
full-SDK phy542xx.c sgmii_autoneg handling for the 54282 system-side register writes.
