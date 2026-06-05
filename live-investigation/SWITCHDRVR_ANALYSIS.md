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

## BCM84758 ucode carve — status

Goal: extract the BCM84758 (10G SFP+ PHY) firmware from here (it's the one PHY
not available in any open SDK — see `PHY_SIGNAL_PATH.md`).

What's established:
- The fw **is** in this binary (`switchdrvr` drives the 84758 live).
- It is **NOT** stored like OpenMDK's legacy `bcm84756_ucode`: the OpenMDK blob
  (32 KB, ends with chip-ID trailer `00 08 47 56`+cksum) is absent, and **no
  `00 08 47 5X` trailer nor `0x000847xx` literal** appears in `switchdrvr`.
- Reason: ICOS uses the **modern phymod** PHY framework (509 `phymod` refs), a
  different generation from OpenMDK's pre-phymod 84756 driver — so the ucode is
  packaged in the phymod form (different layout, likely per-SerDes-core, possibly
  transformed), not the legacy array-with-trailer.

So the carve is a real disassembly task (radare2 is available; no Ghidra on this
host):
1. Locate the phymod 8475x ucode-load routine (xref the `Ucode_Load_Verify*`
   strings at ~`0x217e43c`).
2. Trace the ucode data pointer + length in that routine; carve the blob from
   `.rodata` (the ~15 MB `.rodata` @ `0x01c21078`).
3. Verify against the live PHY's `UCODE_VER`.

### Strategic note — sourcing may beat carving

Because ICOS drives the 84758 via **phymod**, the cleaner fix than carving may be
a **phymod-based SDK that already supports the 84758** (the 8475x phymod core),
rather than the legacy OpenMDK 84756 path. Worth checking newer OpenBCM/phymod
releases or the SAI bcm PHY set for the 8475x phymod core before sinking deep RE
hours into the binary carve. The carve remains the guaranteed-we-own-it fallback.
