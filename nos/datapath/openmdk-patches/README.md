# OpenMDK patches for EdgeNOS-4610 (AS4610-54T / BCM56340 "Helix4")

`edged` links the **OpenMDK** CDK/BMD/PHY libraries. A handful of small, surgical
changes to OpenMDK are needed to bring this exact board up. OpenMDK lives in its
own upstream repo (`github.com/Broadcom/OpenMDK`), so we can't commit into it —
these patches capture the changes and are applied against a fresh clone.

All patches are against the **canonical `PKG/` source** (OpenMDK generates the
`pkgsrc/` build tree from `PKG/` via its chip-install step, so patching `PKG/`
survives a clean rebuild).

## Apply

```sh
cd /path/to/OpenMDK
for p in /path/to/edgenos-4610/nos/datapath/openmdk-patches/0*.patch; do
    patch -p1 < "$p"
done
# regenerate the pkgsrc build tree from PKG (or just re-run build-datapath.sh,
# which rebuilds the affected objects):
```

Or use `./apply.sh /path/to/OpenMDK`.

## The patches

| # | File | What & why |
|---|------|------------|
| 01 | `bcm56340_a0_bmd_attach.c` | **SFP+ → 10G.** `port_speed_max_400[]`: the 4 SFP+ ports (xe0-3) were `3` (2.5G); set to `10` (10G). Surgical — only those 4 entries change, all 48 copper stay identical. Without this `bmd_port_mode_set(10G)` is rejected `CDK_E_PARAM`. |
| 02 | `bcm56340_a0_bmd_switching_init.c` | **Per-port config non-fatal.** A single port's `_config_port` failure no longer poisons `rv`/aborts the loop, so the SFP+ 10G config (TDM still nominal 2.5G) can't take L2/STP down with it (was `0/55`). |
| 03 | `bcm956340k_miim_ext.c` | **Real external-PHY MDIO map.** `_phy_addr()` rewritten to the live `config.bcm` layout: copper jacks 1-24 → EBUS0 `0x01-0x1c`, jacks 25-48 → EBUS1; SFP+ xe0-3 → EBUS2 `0x80-0x83`. The stock formula (gap-of-1) didn't match the board (gap-of-2 + bus split), so the 54282/84758 PHYs never bound. |
| 04 | `bcm84740_drv.c` | **Claim the BCM84758.** The 84758 (AS4610 SFP+ PHY) is 84740-family; teach the bcm84740 driver to match its ID (`0x600d:0x86f0`) and download `bcm84758_ucode` in `init_stage_3`. |

Also set at build time (see `../build-datapath.sh`): `-DPHY_CONFIG_INCLUDE_BCM84740=1`
and `-DPHY_CONFIG_INCLUDE_BCM54282=1` so both drivers are in the probe list.

## Firmware (in this repo — Broadcom source-available)

Patch 04 references `bcm84758_ucode[]`. That firmware array (`bcm84758_ucode.c`,
the 32 KB v0128 8051 ucode) is **included here** — `apply.sh` installs it into
OpenMDK's `phy/{PKG,pkgsrc}/chip/bcm84740/` automatically.

It is Broadcom **source-available** firmware: pulled from Broadcom's own public
GitHub repo `Broadcom/Broadcom-Compute-Connectivity-Software-robo2-xsdk` under its
`Legal/LICENSE`, which grants a worldwide, royalty-free, perpetual right to
reproduce, distribute, and create/distribute derivative works in source form. The
per-file `$Copyright … All rights reserved$` header is Broadcom's standard
boilerplate that the LICENSE supersedes — the same arrangement under which Broadcom
publishes OpenMDK/OpenBCM. The byte-for-byte authoritative source (driver +
firmware + `LICENSE` + `PROVENANCE.md`) lives in
[`../phy84758-src/broadcom-official/`](../phy84758-src/broadcom-official/);
`bcm84758_ucode.c` here is just that firmware with symbols renamed to the OpenMDK
`bcm84758_ucode[]` convention. See `../phy84758-src/INTEGRATION.md`.
