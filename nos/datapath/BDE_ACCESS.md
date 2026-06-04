# BDE Access Layer — how `linux-user-mdk` reaches the Helix4 CMIC

How our datapath app talks to the BCM56340 switch core on the AS4610-54T, and
why it's simpler than the 5610. This is the result of digging into OpenMDK's
`linux-user` BDE + the ONL 4.14 kernel + the board device tree.

---

## The access model (decisive facts)

| Fact | Value | Source |
|---|---|---|
| CMIC register block | **physical `0x48000000`, size `0x40000` (256 KB)** | `bcm-helix4.dtsi` `iproc_cmicd@48000000` |
| DT compatible / IRQ | `brcm,iproc-cmicd` / GIC_SPI 162 | same |
| Chip identity | vendor `0x14e4`, device **`0xb340`**, rev A0 **`0x01`** | OpenMDK `cdk_devids.def` |
| `/dev/mem` access | **open** — `CONFIG_DEVMEM=y`, `CONFIG_STRICT_DEVMEM` **not set** | `armhf-iproc-all.config` |
| Kernel CMIC driver | `drivers/soc/bcm/xgs_iproc/iproc-cmicd.c` (built-in) | ONL 4.14 kernel |

### Why this is fundamentally easier than the 5610

On the **5610** the CPU (PowerPC) reaches the switch (BCM56846) **over PCIe**, so
every access went PCIe BAR0 → **PAXB sub-window remap** → CMICm → SCHAN. That
PAXB layer was the single hardest thing we reverse-engineered.

On the **4610** the ARM CPU **is** the switch's on-die iProc, so the CMIC is a
native peripheral at a fixed AXI address (`0x48000000`). **There is no PCIe and
no PAXB in the path** — you map that one window and you're on the CMIC.

> OpenMDK's `linux-user` example has an iProc code path
> (`_iproc_dev_init` → `shbde_iproc_paxb_init`), but that is for **PCIe-attached
> iProc** switches (host reaches the switch's iProc *through* PAXB). It only runs
> when a *second* base address is supplied. For on-die Helix4 we pass a **single**
> base address and skip PAXB entirely.

---

## Path A — `/dev/mem` PIO (no kernel module) ← use this first

Because `STRICT_DEVMEM` is off, user space can `mmap` the CMIC directly. The
OpenMDK app already does exactly this: `_mmap()` opens `/dev/mem`
(`O_RDWR|O_SYNC`) and maps the physical base; `cdk_dev_create()` then drives the
CMIC. No Broadcom kernel module required for register-level bring-up.

**On the switch:**
```sh
./linux-user-mdk --unit 0x14e4:0xb340:0x01@0x48000000
```
→ maps 256 KB at `0x48000000`, creates the CDK device, drops into the CDK/BMD
shell. From there:
```
init                 # bmd_init: bring the chip out of reset, MMU/port defaults
port                 # show port state
port xe0 speed=10000 # bring up an SFP+ uplink (port 49) ...
# vlan create / vlan add / mac-addr add  -> L2 forwarding
# get/set <reg|mem>  -> raw register/memory poke (for debugging)
```

**What Path A gives:** full register/SCHAN access → `attach`, `init`, SerDes/port
bring-up, L2 table programming, stats, and **polled** packet TX/RX. The app is
already built with `BMD_CONFIG_INCLUDE_DMA=0` (polling), so it needs nothing more.

**What Path A does NOT give:** DMA and interrupts. RX-to-CPU is poll-only and
slow; no IRQ-driven packet path. Fine for bring-up and L2/L3 *control*, not for
production data-to-CPU throughput.

### One thing to validate on hardware (the key unknown)

The theory "map `0x48000000` directly, no PAXB" is correct for an on-die CMIC,
but it's the one assumption that must be confirmed live: after `--unit
…@0x48000000`, a `get` of a known CMIC/SOC register (e.g. the device revision or
`CMIC_*` id register) should read back the expected value. If reads are
all-`0xffffffff`/`0x0`, the mapping/offset model is wrong and we revisit (e.g.
whether the CDK expects an iProc CMIC offset, or the `iproc-cmicd` kernel driver
must be unbound first so it isn't contending).

---

## Path B — kernel BDE (`linux-kernel-bde` + `linux-user-bde`) ← later, for DMA/IRQ

For interrupt-driven DMA and real RX-to-CPU, the Broadcom BDE kernel modules bind
the `brcm,iproc-cmicd` platform device (the kernel already exposes it; the in-tree
`iproc-cmicd.c` provides the platform anchor — the `0x48000000` region + IRQ 162)
and present `/dev/linux-kernel-bde` + `/dev/linux-user-bde` with `mmap` + IRQ +
DMA buffers. The user app then opens those instead of `/dev/mem`.

These modules are **not in OpenMDK** (its libbde is only the userspace `shbde`
helper). They come from the full SDK (**OpenBCM** `gpl-modules` / `dcb-gpl-modules`,
GPLv2). Plan when we get there:
1. Build `linux-kernel-bde.ko` + `linux-user-bde.ko` from OpenBCM against the ONL
   4.14 source tree (`.../linux-4.14.151`, already on disk) with the
   `arm-linux-gnueabihf` toolchain — same pattern as `build-datapath.sh`.
2. Point the user app at the BDE devices (the OpenBCM `linux-user` harness, or
   port our `linux_shbde.c` to the kernel-BDE ioctls).
3. This also unlocks the iProc CMICd DMA model (continuous DCB ring), the thing
   we decoded painfully on the 5610.

> Keep the GPL boundary (see `../LICENSING.md`): the BDE `.ko`s are GPLv2 and live
> in-kernel; the proprietary SDK stays in userspace.

---

## Recommended sequence

1. **Path A, register sanity:** `--unit …@0x48000000`, `get` a known reg → prove
   we're on the CMIC. (No kernel module — fastest possible first contact.)
2. **Path A, chip up:** `init`, then bring one 1G copper + one 10G SFP+ port to
   link (Warpcore ucode is compiled in); verify with `port`/stats.
3. **Path A, L2:** VLAN + MAC + forward between two front ports; polled CPU tx.
4. **Path B:** build the BDE `.ko`s against 4.14 → DMA/IRQ → real RX-to-CPU.
5. Wrap into a daemon; add L3 (OpenBCM `bcm_l3_*` or hand-rolled table fills using
   the CDK defs) + FRR.

The big point: **step 1 needs nothing but our already-built `linux-user-mdk` and
`/dev/mem`** — so the moment the box is installed and on console, we can make
first contact with the silicon with zero additional build work.
