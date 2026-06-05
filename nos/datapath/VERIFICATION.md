# Datapath Verification — our NOS vs the live stock ICOS NOS

Cross-check of what **our** NOS (`nos/`) needs to drive the BCM56340, against the
**ground truth** captured from the running Edgecore FASTPATH/ICOS box
(`../../live-investigation/`). Verdict: **we have what we need** — the live
analysis confirmed our assumptions and filled the one real gap (board config).

## Verdict table

| What our NOS needs | Have it? | What the live box confirmed / gave us |
|---|---|---|
| **CMIC access address** | ✅ | ICOS maps CMIC at kernel VA `0xa698e000` = ioremap of **phys `0x48000000`** — exactly our `BDE_ACCESS.md` value. |
| **Chip identity for `--unit`** | ✅ | `soc` → `Chip=BCM56340_A0 Rev=0x01` → our `--unit 0x14e4:0xb340:0x01@0x48000000` is correct. |
| **Chip driver** | ✅ built | OpenMDK `bcm56340_a0` BMD, cross-compiled (`linux-user-mdk`). |
| **Port map** | ✅ confirmed | `ps` → `ge0–47` (1G SGMII) + `xe0–3` (10G) + `xe4–5` (20G) + `ge48` (CPU/GMII). Matches our hardware exactly. |
| **48× 1G copper PHY driver** | ✅ in binary | PHY is **Broadcom BCM5464S** (`phy_5464S_*` in config) → OpenMDK `bcm5464` driver **is linked** (`PHY_CONFIG_INCLUDE_BCM5464`, `BCM5464_AUTO_DETECT_SGMII_MEDIA`). |
| **10G/20G uplink SerDes** | ✅ in binary | Warpcore (`wc40_ucode_b0`) linked; `phy_fiber_pref_xe*=1`. |
| **MDIO/MIIM bus** | ✅ in binary | `bcm56340_miim_int` linked (PHY register access). |
| **Board config (`config.bcm`)** | ✅ **now have it** | The one thing our chip-generic build lacked — extracted live to [`config.bcm.as4610-54t`](config.bcm.as4610-54t): 144 props incl. **per-port PHY MDIO addrs**, `pbmp_xport_ge/xe`, `bcm56340_4x10=1`, table sizes. |
| **DMA model (Path B)** | ✅ known | `soc` → **DCB type 23**, ch0=TX / ch1=RX, DV ring 32 (`dv-size=160`). (Path A/PIO needs none of this.) |
| **Kernel BDE (Path B)** | ✅ source | ICOS uses `linux-kernel-bde` + `linux-user-bde` (lsmod) — our planned Path B; source in OpenBCM (GPLv2). Device nodes maj 127/126. |
| **Table sizes / scale** | ✅ known | `l2_mem_entries=0x4000` (16K L2), `l3_mem_entries=0x2000` (8K L3), `l3_max_ecmp_mode=1`, `ipv6_lpm_128b_enable=1`, `bcm_num_cos=8`. |

## The one integration item

We have every *piece*; the remaining work is **feeding the board `config.bcm`
into the datapath**, because OpenMDK's BMD (unlike the full SDK) does **not**
auto-read a `config.bcm` file — it expects per-port PHY addresses from board
code. Two clean options, both now unblocked by the extracted config:

1. **OpenMDK + supply the map** — pass the `port_phy_addr_geN` / `xeN` values
   (from `config.bcm.as4610-54t`) into the BMD's PHY-bus setup (board code /
   `bmd_phy_ctrl`). Smallest footprint; matches our current `linux-user-mdk`.
2. **OpenBCM (full SDK)** — consumes `config.bcm.as4610-54t` **directly** (it's
   real config.bcm syntax). Heavier, but also gives L3 and is what ICOS itself
   uses. Best if/when we want the full router.

Either way, the data we were missing now exists verbatim from the live box.

## Confirmed bring-up sequence (no surprises expected)

1. `./run-on-target.sh` → `linux-user-mdk --unit 0x14e4:0xb340:0x01@0x48000000`
   (Path A, `/dev/mem`) → register read-back sanity (we now know CMIC is there).
2. `init` → chip out of reset.
3. Apply the board config (option 1/2 above) so PHY addresses are known.
4. Bring up a 1G copper port (BCM5464S @ its `port_phy_addr`) and a 10G SFP+
   (`xe0`, Warpcore) → link.
5. L2 (VLAN/MAC/forward), CPU tx (polled).
6. Path B (kernel BDE, DCB t23) for DMA/IRQ → real RX-to-CPU.
7. L3 (OpenBCM `bcm_l3_*`, or hand-rolled) + FRR.

## Bottom line

Nothing is missing to start bring-up. The live ICOS analysis **validated** our
access model and port map, **confirmed** the PHY/SerDes drivers are already in
our binary, and **supplied the board `config.bcm`** that a chip-generic SDK
build can't know on its own. Remaining = integration + on-hardware execution,
not discovery.
