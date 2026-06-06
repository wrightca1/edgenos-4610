# BCM84758 / 84740-family PHY source — PROVENANCE & LICENSE

These files were pulled **2026-06-05** directly from **Broadcom's own official
GitHub organization**:

> `github.com/Broadcom/Broadcom-Compute-Connectivity-Software-robo2-xsdk`
> path `robo2-xsdk/src/soc/phy/`  (commit on default branch; repo pushed 2020-08-03)

| File | Bytes | Role |
|---|---|---|
| `phy84740.c` | 324971 | driver (defines `PHY84740_ID_84758 0x84758`; loads `phy84758_ucode_bin`) |
| `phy84758_ucode.c` | 205334 | **BCM84758 firmware**, v0128, 32768-byte 8051 ucode |
| `phy84740_ucode.c` | 199285 | sibling family firmware |
| `LICENSE` | 10625 | the Broadcom/Avago grant (copied from repo `Legal/LICENSE`) |

## Why this copy, not the earlier mirror

The first copy we obtained came from an **unofficial mirror** (`sysdevguru/…`,
SDK 6.5.7) whose file header read `Broadcom Proprietary and Confidential. All
rights reserved.` with **no LICENSE file in the repo** — legally murky.

These official files instead carry the header:

```
* This license is set out in https://github.com/Broadcom/.../Legal/LICENSE file.
* $Copyright: (c) 2020 Broadcom Inc.
```

i.e. Broadcom's own publication explicitly points the header at a **granting**
license and drops the bare "all rights reserved" wording.

## The grant (Legal/LICENSE, verbatim key clause)

> "Broadcom grants to Licensee a worldwide, non-exclusive, royalty-free, perpetual
> license (i) to evaluate, use, reproduce, publicly display, publicly perform, and
> distribute the Software, and (ii) for Software provided in source code form, in
> addition to the rights in (i), to create derivative works and distribute such
> source code and any derivative works. Licensee may sublicense any or all of the
> foregoing rights to third parties subject to the terms of this Agreement."

This is **word-for-word the same grant as OpenMDK/OpenBCM** (see ../../LICENSING.md).
So: source-available, redistributable, derivatives allowed.

## Conditions we must honor (from the same LICENSE)

1. **Preserve all proprietary notices** — keep the copyright/license header on
   every copy (do not strip it). This `LICENSE` file travels with the sources.
2. **No reverse-engineering of object code** — irrelevant to *these* (they are
   source), but it is why we DO NOT rely on the ICOS `switchdrvr` carve as a
   distribution source; the carve was internal verification only.
3. **NOT FSF/OSI free, GPL-incompatible** — field-of-use limits + notice rules.
   Keep the kernel-module split (proprietary SDK in userspace; GPLv2 BDE/KNET in
   kernel) — same rule as OpenBCM. Do not statically combine into GPL'd code and
   relabel as GPL.
4. **Export control** — the SDK/firmware may be subject to U.S. EAR; comply.
5. "AS IS", no warranty, no support; California law; liability capped.

## Verification

The 32768-byte firmware blob extracted from `phy84758_ucode.c` here is
**byte-for-byte identical** to (a) the unofficial mirror and (b) the copy carved
from our own AS4610 ICOS `switchdrvr` @ offset 0x34de1bc:

```
sha256 = 64ae5619b3625982141250299c573e734663133547c18f0bf29317ea0112f9cf
```

So all three are the same firmware; this official copy is just the one with a
clean, redistributable license of record.
