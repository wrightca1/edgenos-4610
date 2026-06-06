# AS4610-54T NOS — Licensing of the Switch SDKs

Licensing of the two Broadcom SDKs we'd build a data plane on (OpenMDK, OpenBCM),
plus the open components around them. **Not legal advice** — read the actual
`Legal/LICENSE` files before shipping.

> One-line summary: both SDKs are **source-available and freely
> redistributable (including derivative works and sublicensing), royalty-free
> and perpetual — but NOT OSI/FSF "free software."** You can legally build and
> ship a NOS on them; you cannot call the data plane libre, and you cannot
> relicense it under the GPL.

---

## OpenMDK — one license (`Legal/LICENSE`)

Licensor: **Avago Technologies International Sales Pte. Ltd.** (a Broadcom
subsidiary). A single agreement covers the whole tree (CDK, xgsm, libbde, phy,
bmd, SerDes firmware).

**Grant (verbatim):**
> "Broadcom grants to Licensee a worldwide, non-exclusive, royalty-free,
> perpetual license (i) to evaluate, use, reproduce, publicly display, publicly
> perform, and distribute the Software, and (ii) for Software provided in source
> code form, in addition to the rights in (i), to create derivative works and
> distribute such source code and any derivative works."

Sublicensable to third parties.

**Key restrictions / terms:**
- No reverse-engineering / deriving source from *object-code-only* software
  (moot — OpenMDK ships as source, so you already have it).
- "Modify" is granted only via the **derivative-works** right, which applies to
  source-form software (i.e. all of OpenMDK) — so modifying the source is OK.
- Must retain proprietary notices; no high-risk uses (medical/nuclear/aviation/
  military); export-control compliance.
- AS-IS, no warranty/support; liability capped at greater of amount-paid or
  **US $1**; governed by California law.

---

## OpenBCM (sdk-6.5.27) — three tiers

The `Legal/` dir was absent from our local checkout; source headers point to the
canonical file:
`https://github.com/Broadcom-Network-Switching-Software/OpenBCM/blob/master/Legal/LICENSE`
(fetched + verified). Copyright 2007–2022 Broadcom Inc.

| Tier | Scope | License |
|---|---|---|
| **Main SDK** | `src/`, `include/` — the L3 API, CDK, `soc/esw/helix4.c`, etc. | **Same Broadcom source-available license as OpenMDK** — grant clause is *word-for-word identical* (worldwide, non-exclusive, royalty-free, perpetual; derivative works for source). Not OSI/FSF. |
| **Kernel modules** | `gpl-modules/`, `dcb-gpl-modules/` — BDE / KNET / DCB | **GPLv2** — genuine OSI/FSF open source. |
| **Third-party components** | bundled OSS | Their own upstream licenses — see `Legal/EXTLICENSE`. |

---

## What it means for our NOS

| Component | License | OSI/FSF-libre? | Ship a NOS on it? |
|---|---|---|---|
| OpenMDK (CDK + bmd + phy + firmware) | Broadcom/Avago source-available | ❌ | ✅ redistribute + derivatives granted |
| OpenBCM main SDK | Broadcom source-available (same terms) | ❌ | ✅ same grant |
| OpenBCM kernel modules | **GPLv2** | ✅ | ✅ |
| FRR, Linux kernel, ONLP, Debian/Buildroot | GPL/EPL/etc. | ✅ | ✅ |

### Three practical rules

1. **Distribution is allowed.** Both SDKs explicitly grant royalty-free
   redistribution and derivative works, including the source. This is the legal
   basis ONL / SONiC / Cumulus rely on, and it lets us publish our own NOS source.
2. **Don't claim a libre data plane.** Neither main SDK is FSF/OSI free software.
   Our stack is *"source-available, vendor-NOS-free"* — see
   [`FULLY_OPENSOURCE_LINUX_PLAN.md`](FULLY_OPENSOURCE_LINUX_PLAN.md).
3. **Respect the GPL boundary.** The Broadcom license is **GPL-incompatible**
   (field-of-use limits, notice requirements). That is *why* OpenBCM isolates the
   kernel modules as separate GPLv2 dirs — so they can lawfully link the GPL'd
   Linux kernel while the proprietary SDK stays in userspace. Keep that split in
   any design: **proprietary SDK in userspace; GPLv2 BDE/KNET shims in-kernel.**
   Do not statically combine the SDK into GPL'd code and redistribute as GPL.

> Note the asymmetry: the SerDes firmware "blob" problem common to open switches
> is *milder* here — OpenMDK ships the Warpcore microcode as **source** under the
> same redistributable license, not as an opaque binary.

---

## BCM84758 PHY driver + firmware (10G SFP+) — source-available after all

This is the one piece our open stack originally lacked (the 10G SFP+ PHY on
xe0–3; see [`live-investigation/PHY_SIGNAL_PATH.md`](live-investigation/PHY_SIGNAL_PATH.md)).
We first obtained it from an unofficial mirror whose `All rights reserved` header
(no LICENSE file) made it look proprietary — but that was a sourcing artifact.

**Corrected finding (2026-06-05):** the identical driver + firmware are published
by **Broadcom's own official GitHub org**,
`Broadcom/Broadcom-Compute-Connectivity-Software-robo2-xsdk`, under a
`Legal/LICENSE` whose grant is **word-for-word the OpenMDK/OpenBCM grant**
(worldwide, non-exclusive, royalty-free, perpetual — use/reproduce/distribute +
derivatives for source). So it sits in the **same tier as the rest of our stack**.

| Item | License | Redistribute? |
|---|---|---|
| `phy84740.c`, `phy84758_ucode.c`, `phy84740_ucode.c` — re-sourced to `nos/datapath/phy84758-src/broadcom-official/` (+ `LICENSE`, `PROVENANCE.md`) | Broadcom source-available, **same grant as OpenMDK/OpenBCM** | ✅ Yes, under the grant's conditions |

Conditions (same as the rest of the SDK): **preserve the proprietary notices**
(keep the header + ship the LICENSE), **GPL-incompatible** so keep the
userspace/kernel split, comply with **export control**, no warranty.

- **Use the official copy, not the mirror.** The official header points at the
  granting LICENSE; the mirror's bare "All rights reserved" header does not.
- **Verified byte-identical (SHA256 `64ae5619…0112f9cf`, v0128, 32768 B)** across:
  Broadcom official repo, the mirror, **and** the blob carved from our own ICOS
  `switchdrvr` (offset `0x34de1bc`). The carve is internal verification only — we
  do **not** rely on reverse-engineering object code, which the LICENSE forbids;
  we use the source Broadcom itself distributes.
- **Publishable-NOS note:** while the grant *permits* redistributing the source,
  the cleanest packaging is still the Linux `request_firmware()` split (ship the
  driver; load the blob at runtime) — but that's a hygiene choice now, not a legal
  requirement.
