# ICOS `switchdrvr` — what this binary is (and the BCM84758 carve status)

`switchdrvr` (58 MB armhf ELF, the ~756 MB-RSS process supervised by `procmgr`)
is the **monolithic switch-control core of ICOS** — the Edgecore/FASTPATH
analogue of Cumulus's `switchd`. Extracted from the live box to
`backup/icos-extract/switchdrvr` (git-ignored, proprietary).

## What it does (from strings/section analysis)

It bundles, in one process:

1. **The full Broadcom XGS SDK** — the complete chip stack:
   - `soc_` low-level chip access (1860 refs), `bcm_` API (6416), **`phymod`** modern
     PHY framework (509) + legacy `_phy_` (1171).
   - Datapath features: `mmu` (1954), `field_`/ACL (2320), `vlan` (2426), `l3_` (522),
     `l2_` (326), `cosq` (439), `mirror` (423), `trunk` (327), `mpls` (329).
   - The **diag shell** (`bcmsh`/drivshell): `getreg`/`setreg`/`listmem`/`port`/`init`/…
   - PHY drivers + **firmware** for the board's PHYs (BCM54282 1G, **BCM84758** 10G,
     Warpcore B1 QSFP) — this is where the 84758 ucode lives.
   - Packet I/O / CMIC DMA.
2. **FASTPATH HAPI** — the `hapiBroad*` hardware-abstraction layer that turns ICOS
   feature requests into BCM SDK calls (the bridge between the apps and the chip).
3. **OpenFlow + Open vSwitch** — yes, an OpenFlow agent + OVS components
   (`/vendor/openvswitch/...`, "Active OpenFlow connection methods", OpenFlow
   port DB). ICOS has an OpenFlow mode.
4. Links the routing/mgmt libs (`libospf`, `libvr_agent`, `librestconf`,
   `libping`, `libtraceroute`, …) — though OSPF/etc. run as separate `*_app`
   processes that talk to `switchdrvr` (the central HAL) via IPC.

In short: **chip SDK + PHY firmware + HAPI + diag shell + OpenFlow/OVS + packet
I/O**, all in one binary. It's the piece our own NOS replaces with
OpenMDK/OpenBCM + an agent.

## BCM84758 ucode carve — ✅ DONE (2026-06-05), and it was trivial

Goal: extract the BCM84758 (10G SFP+ PHY) firmware from here (it's the one PHY
not available in any open SDK — see `PHY_SIGNAL_PATH.md`).

**Result: the 32768-byte ucode is stored contiguously and in the clear** in this
binary at offset **`0x34de1bc`**, found by pure content match — **no disassembly
needed.** It is byte-for-byte identical (SHA256
`64ae5619…0112f9cf`, 0 diffs) to the mirror's `phy84758_ucode.c` (v0128). Carved
copy: `backup/icos-extract/phy84758_ucode_carved.bin`.

### Correcting the earlier (wrong) pessimism

The earlier analysis assumed this would be a hard radare2/Ghidra carve because:
- "the OpenMDK `bcm84756_ucode` trailer `00 08 47 5X` is absent" — **true but
  misleading.** The 84758 is **84740-family**, so its trailer is `00 08 41 64
  01 28 7d 7c` (84740 chip-ID + version 0128 + cksum). Searching for the *84756*
  trailer naturally missed it; searching for the actual firmware bytes finds it
  instantly.
- "ICOS uses modern phymod so the ucode must be transformed/per-core" — **wrong.**
  The 8051 ucode array is stored verbatim in `.rodata`, exactly as in the SDK C
  source, regardless of the phymod wrapper around it. The neighboring offsets
  (`0x34ae84c`, `0x34b8034`, …) are the rest of the 84740-family ucode set.

So no `Ucode_Load_Verify` xref-tracing was required. The carve is a one-liner
content search; the binary-extract route is now a **confirmed, owned** source of
the firmware, not a last resort.

## MDIO RE (2026-06-05) — front PHYs are on the CMIC MIIM; param-encoding mismatch
Traced how ICOS reads the BCM84758 (10G SFP+) MDIO:
  portmod_ext_phy_mdio_c45_reg_read (0x18484c4) -> soc_esw_miim_read (0x184705c)
  -> soc_miim_read (0x184683c) [+ soc_miim_modify 0x1847065, soc_miim_write 0x1846120]
=> ICOS reaches the EXTERNAL front-panel PHYs over the **CMIC MIIM** (soc_miim), NOT the iProc
CCB MDIO (that's the mgmt PHY) and NOT a board mux. My earlier "board MDIO mux" hypothesis was WRONG.

soc_miim_read decodes the phy_addr into CMIC_MIIM_PARAM bit-fields explicitly:
  bits[4:0]   = device address       (and r2, fp, 0x1f)
  bits[6:5]   = ? (ubfx sb, fp,5,2)  -> feeds param
  bit7        = internal/clause sel  (tst fp,0x80 -> orr param, 0x4000000  i.e. PARAM bit 26)
  bits[9:8]   = bus number           (and r0, fp, 0x300)
OpenMDK cdk_xgsm_miim instead does a flat `phy_param = phy_addr<<16`, which works for INTERNAL
(IBUS2=0x280 happens to decode right -> Warpcores found) but mis-places the bus/select bits for
EXTERNAL -> front PHYs never answer. THAT is why my external CMIC-MIIM scan found nothing.

FIX (next): in edged, do the CMIC MIIM C45 read directly against the CMIC (0x48000000) CMC MIIM
regs (CMIC_CMC_MIIM_PARAM/ADDRESS/CTRL/READ_DATA) using ICOS's param encoding (bits 4:0 dev, 9:8
bus, bit7->param bit26, + C45 devad/clause), scan for 0x600d:* front PHYs. Reference: soc_miim_read
@ 0x184683c. No board mux, no iProc CCB MDIO needed for the switch-port PHYs.

### Update: ICOS PARAM-encoding replication tried — still no front PHYs
Added edged CMIC-MIIM scan using the RE'd PARAM encoding (cdk_addr = dev|(bus<<6)|(bit7<<10);
xe0->0x481, ge0->0x401), swept bus/dev, C45. Front PHYs STILL don't answer. So the PARAM
encoding alone is insufficient — soc_miim_read's PREAMBLE (0x184684e-0x184688c: reads soc-struct
flags at +0xcaf344/+0xcaf3a4/+0xcaf3ac/+0xcaf378, calls 0x180d090 conditionally, + lock
0x1416024) almost certainly ENABLES/sets-up the external MDIO master before the transaction —
a step bmd_init/our code doesn't do (bmd_init only set CMIC_RATE_ADJUST clock). NEXT: fully
decode soc_miim_read's preamble (esp. the 0x180d090 call + the 0xf3ac flag gate) to find the
external-master enable register write, replicate it, then the PARAM-encoded read should work.
This remains 5610-class deep RE (each layer reveals another).

### Update #2: NOT param encoding — external MDIO MASTER is disabled (needs enable/routing)
Swept cdk_xgsm_miim C45 phy_addr 0x000-0x3ff (ALL bus/select bits): only internal Warpcores
(0143:bff0, addrs 0x281-0x2bf, bit9=internal-select). EVERY external-bus addr (bit9 clear) is
silent. So PARAM encoding is NOT the problem — the external MDIO master is not enabled/routed.
- bmd_init sets CMIC_RATE_ADJUST (ext MDIO *clock*) but nothing enables the master.
- CMIC_MIIM_BUS_MAP/BUS_SEL_MAP regs are bcm56840(Trident)-only; bcm56340(CMICm) lacks them.
- On this iProc Helix4 the CMIC external MDIO is routed/enabled via the iProc side (pinmux /
  CCB-MDIO bridge) by soc_reset_bcm56340_a0 / soc_misc_init (large funcs, not yet decoded).
TWO realistic paths to the enable register:
  (A) Systematic RE: decode soc_reset_bcm56340_a0 (0x147aa1c) + soc_misc_init (0x17e5e34) for the
      iProc/CMIC external-MDIO enable+route writes. Safe, slow, multi-session.
  (B) Empirical diff: restore+boot ICOS (backup verified, RESTORE.md), dump CMIC(0x48000000)+iProc
      MDIO(0x18032000) regs while external MDIO works, reflash our NOS, diff vs our state -> the
      enable bit(s) pop out. Decisive + fast, but costs a reflash cycle (disruptive).
Everything else for SFP+ is staged (driver+ucode+scanner). This is the 5610-class PHY battle.

### Update #3: ICOS register-capture diff (option B) — bus-map found, but not sufficient
Restored+booted ICOS (verified backup), confirmed 84758 works (phy info: xe0 600d:86f0 addr 40),
dumped CMIC+iProc regs via linuxsh/devmem, reflashed our NOS, diffed. Captures in
dumps/icos_mdio_regs.txt. Findings:
- iProc CCG (0x1803fc00) + 0x1803f000 + RATE_ADJUST clocks: IDENTICAL ICOS vs ours.
- DIFF is entirely in CMIC region 0x48011000 — the MIIM bus map:
    0x48011074-84 (bus internal/external SELECT map): ICOS 0x09249000/09249249/03249249/00092480/2;
      ours ALL ZERO.
    0x48011094-bc (bus->address MAP, octal-PHY gaps): ICOS skips 09/0a; ours naive-sequential.
    + config 0x008(0x30001000 vs 0x20001000), 0x010, 0x058/060/064 (ICOS set, ours 0).
- edged write_icos_miim_map() replays all 22 differing regs after init (verified they stick).
  RESULT: external MDIO STILL dead — only internal Warpcores answer. So the bus map is necessary
  but NOT sufficient.
REMAINING (next): (a) external PHY RESET — 84758/54282 likely held in reset; ICOS deasserts via
CPLD(i2c 0x30)/GPIO during init (not captured — would need another ICOS boot to dump CPLD+GPIO, or
RE soc_reset_bcm56340_a0 for the de-reset writes). (b) bus map may need programming at MIIM-init
time (before MIIM use), not post-init. This is the genuine remaining wall.

### ✅✅ SOLVED 2026-06-05 — the 84758 answers! Complete external-MDIO recipe
After replaying the CMIC MIIM bus map, the FINAL missing piece was the external PHY RESET:
our NOS left CPLD 0x19=0x7f / 0x1b=0xb5 (PHYs HELD IN RESET — the power-on default; ONL's
accton_as4610_cpld driver only writes 0x2A for QSFP, never 0x19/0x1b). ICOS clears them to 0x00.
Clearing 0x19=0x00 + 0x1b=0x00 (i2cset -y -f 0 0x30) -> the 84758 (600d:86f0) appears at CMIC MIIM
EBUS2 addr 0-3 (miim 0x080-0x083) AND all 54282 1G (600d:845b) at 0x01-0x1f. EXTERNAL MDIO WORKS.

COMPLETE external-MDIO enable recipe (what ICOS does that our bmd_init doesn't):
  1. CMIC MIIM bus map @0x48011000 (edged write_icos_miim_map: 0x074-084 sel-map + 0x094-0bc
     addr-map + cfg 0x008/010/058/060/064).  [/dev/mem]
  2. Deassert external PHY reset: CPLD(i2c0 0x30) 0x19=0x00, 0x1b=0x00 (+ 0x07=0x02,0x08=0x02,
     0x0d=0x01 also set by ICOS).  [i2c]
Then a CMIC MIIM C45 read at the right phy_addr reaches the 84758. The 84740-family driver (already
patched for the 84758 ID + ucode) can now probe+bind+download. NEXT: reorder edged so bus-map+CPLD
deassert run BEFORE bmd_attach (so the PHY probe at attach finds the 84758 and downloads the ucode);
verify firmware loads (PHY reg 1.0xca1c==0x600d). Still needs a 10G module to confirm actual link.
