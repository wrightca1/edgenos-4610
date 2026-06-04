# Data Plane — EdgeNOS-4610 switching agent

ONL gives us a booting Linux + platform management (sensors/optics/LED/fan), but
**not** packet forwarding. The data plane is a separate component that drives the
BCM56340 silicon through the Broadcom SDK. This directory will hold that agent.

## What's needed

1. **Kernel BDE module(s)** — bus/DMA enumeration + CMICd access, built against
   the ONL **iProc 4.14** kernel.
   - Source: OpenBCM `gpl-modules` / `dcb-gpl-modules` (GPLv2) or OpenMDK `libbde`.
   - On Helix4 the CMIC is on-die (`iproc_cmicd`) — **no PCIe/PAXB**, unlike the
     5610. Simpler probe.
2. **User-space SDK** — the chip driver.
   - **Phase A (L2):** OpenMDK `bcm56340_a0` BMD — init, ports (incl. 10G SFP+/
     QSFP+ via Warpcore SerDes ucode), VLAN, STP, L2, TX/RX, stats. Fastest proof
     the chip forwards.
   - **Phase B (L3):** OpenBCM `bcm_l3_*` (route/host/nexthop/ECMP/L3-intf) — or
     hand-rolled table fills using OpenMDK's CDK defs (`bcm56340_a0_defs.h`) +
     `xgsm_mem_write`. The full L3 silicon definitions are already provided; see
     `../../EXISTING_SOFTWARE_ASSETS.md`.
3. **Agent** — a small daemon: init chip → configure ports → program L2/L3 →
   bridge the control plane (netlink FIB / link events) to the SDK.
4. **FRR glue** — `zebra` FIB → agent → chip (a SONiC-`fpmsyncd`-style sync, or a
   minimal netlink listener).

## Bring-up order (de-risks the hardest piece early)

1. Build BDE module against the 4.14 kernel; load on the box; enumerate the chip.
2. OpenMDK `bmd_attach` + `bmd_init` → confirm chip alive (register reads).
3. **Ports + SerDes**: link a 1G copper port and a 10G SFP+ (Warpcore ucode is
   in OpenMDK as source). This is the riskiest hardware step — do it first.
4. L2: VLAN + learn + forward between two front ports.
5. RX/TX to CPU (punt path) — validate XGS-M DCB on this stepping.
6. L3: program an L3 interface + static route; route *through* the box.
7. Wire FRR; bring up OSPF/BGP.

## Key references (parent folder)

- `EXISTING_SOFTWARE_ASSETS.md` — the OpenMDK CDK/xgsm/PHY/BMD layer breakdown.
- `ASIC_AND_CPU_ARCHITECTURE.md` — why Helix4's on-die CMIC removes the 5610's
  PAXB/PCIe access problems.
- `LICENSING.md` — keep the proprietary SDK in userspace; GPLv2 BDE in-kernel.

## Status

**Built (2026-06-04): `mdk-app/linux-user-mdk` — armhf datapath tool.** ✅

A cross-compiled OpenMDK CDK/BMD shell for the AS4610-54T, our first on-box
datapath binary:

- `ELF 32-bit LSB pie, ARM EABI5, interpreter /lib/ld-linux-armhf.so.3` — matches
  the ONL stretch/armhf rootfs, so it runs on the installed NOS as-is.
- **Scoped to BCM56340** (2,136 chip refs; other chips excluded).
- Links the **iProc CMIC** path (`cdk_xgsm_cmic_init`, `shbde_iproc`) — the
  on-die access stack, no PCIe/PAXB (unlike the 5610).
- BMD command set: `attach` / `init`, `port_mode_set` + XLPORT/SerDes bring-up,
  `vlan`, `mac-addr` (L2), packet `tx`, register/memory `get`/`set`.

### Build it

```bash
./build-datapath.sh        # → mdk-app/linux-user-mdk (armhf)
```
Compiles inside the `edgenos/builder9:1.8-rootless` image (same
arm-linux-gnueabihf toolchain + stretch glibc as the NOS). Source/config live in
`mdk-app/` (copied from OpenMDK `examples/linux-user`, with `cdk_custom_config.h`
scoped to BCM56340). Build notes baked into `build-datapath.sh`:
MDK env, `MAKE=make` (image has no gmake), `-Wno-error` (old code vs gcc 6.3),
`SYS_BE_*=0` (ARM little-endian).

### On-box bring-up plan (once installed + console)

1. Copy `linux-user-mdk` to the switch; run it → CDK/BMD shell.
2. `attach` the BCM56340 over the iProc CMIC (needs the BDE kernel module / the
   ONL `iproc_cmicd` device — to be wired next).
3. `init` the chip → confirm via register reads.
4. Bring a 1G copper port + a 10G SFP+ to link (Warpcore ucode is in OpenMDK).
5. L2: VLAN + MAC + forward between two ports; CPU tx.
6. Then graduate to a daemon (this is a shell/diag tool first) and add L3.

### BDE access — RESOLVED (see [`BDE_ACCESS.md`](BDE_ACCESS.md))

How the app reaches the chip is figured out:

- **CMIC is at physical `0x48000000` (256 KB)** on the SoC (DT `iproc_cmicd`) —
  on-die, **no PCIe/PAXB** (the big 5610 headache is absent here).
- The ONL 4.14 kernel has **`CONFIG_STRICT_DEVMEM` unset**, so the app can
  `mmap /dev/mem` at `0x48000000` directly — **no kernel module needed** for
  register-level bring-up. Run it with:
  ```sh
  ./linux-user-mdk --unit 0x14e4:0xb340:0x01@0x48000000   # or: ./run-on-target.sh
  ```
- DMA/IRQ (real RX-to-CPU) later needs the Broadcom BDE `.ko`s (OpenBCM GPLv2)
  built against the 4.14 kernel — "Path B" in `BDE_ACCESS.md`.

`run-on-target.sh` is the copy-to-switch wrapper for first contact.

### Still TODO (phase-2 cont., mostly needs hardware)

- Validate Path A live: register read-back proves we're on the CMIC.
- Bring up ports (1G + 10G), L2 forwarding.
- Path B: build `linux-kernel-bde`/`linux-user-bde` from OpenBCM vs 4.14 → DMA/IRQ.
- Turn the diag shell into a long-running agent + netlink/FRR glue (L3 phase).
