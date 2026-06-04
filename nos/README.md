# EdgeNOS-4610 — a NOS for the Edgecore AS4610-54T

Building our own network OS for the **AS4610-54T** (Broadcom BCM56340 "Helix4"
SoC, dual Cortex-A9, armhf). Sibling to the AS5610-52X effort, but a far easier
target — see the docs in the parent folder for the hardware/SDK background.

## Chosen approach (decided 2026-06-04)

| Decision | Choice | Why |
|---|---|---|
| **Base** | **ONL-derived** | ONL already has the full platform layer (DTS, CPLD/SFP/PSU/fan/LED drivers, ONLP) for `as4610-54`. Don't reinvent it. |
| **Kernel (phase 1)** | **ONL iProc 4.14** (armhf, stretch suite) | Known-good for this exact board; BDE + SDK build against it. Boot fast, prove the board + datapath. |
| **Kernel (phase 2)** | **Forward-port to latest mainline LTS (6.x)** | The XGS-iProc SoC family (Hurricane2/`ARCH_BCM_HR2`) is upstream; adapt to Helix4. See [`docs/KERNEL_FORWARD_PORT_6X.md`](docs/KERNEL_FORWARD_PORT_6X.md). |
| **Data plane** | **Broadcom SDK** (OpenMDK first, OpenBCM for L3) + thin agent | Only way to drive Helix4 silicon. See [`datapath/README.md`](datapath/README.md). |
| **Routing** | **FRR** on top | Static/OSPF/BGP/VRRP; pushes routes to the agent. |

Net: **ONL gives us a booting armhf Linux with working sensors/optics/LEDs; we
add the switching data plane (L2 now, L3 next) and FRR.** The board's hardware is
L2+L3 capable (datasheet + Helix4 silicon); the work is software.

## Layout

```
nos/
├── README.md                      ← this file
├── build.sh                       ← one-command build driver (ONL armhf as4610-54)
├── BUILD_ENV.md                   ← build prerequisites + environment status
├── scripts/                       ← helper scripts (install, console, etc.)
├── datapath/                      ← switching agent (SDK/BDE integration)  [phase 2]
│   └── README.md
└── docs/
    └── KERNEL_FORWARD_PORT_6X.md  ← the 4.14 → mainline-6.x roadmap        [phase 3]
```

The ONL source tree itself lives at `../../OpenNetworkLinux/` (shared, not copied
in here). `build.sh` drives it.

## Quick start

```bash
# 1. one-time: see BUILD_ENV.md (submodules + builder image)
# 2. build the armhf ONIE installer for the AS4610-54T:
./build.sh
# → ../../OpenNetworkLinux/RELEASE/stretch/armhf/ONL-*ARMHF*INSTALLER
```

Then ONIE-install over the network (the box is ONIE-equipped; boots NOS from
`/dev/sda`). Console: 115200 8N1 on `ttyS0` (serial rig pending).

## Status

- [x] Approach decided; build environment prepared (submodules + patched rootless builder).
- [x] **AS4610-54 platform packages build clean** (kernel 4.14.151, ONLP, modules, platform-config, DTB) — `REPO/stretch/packages/binary-armhf/`.
- [x] **Full armhf ONIE installer built** (rootful Docker for the chroot stage) — `RELEASE/stretch/armhf/ONL-master_ONL-OS9_2026-06-04.*_ARMHF_SWI_INSTALLER` (149 MB, md5 OK).
- [ ] Serial console + ONIE install onto the unit.
- [x] **Datapath tool cross-compiled** — `datapath/mdk-app/linux-user-mdk` (armhf OpenMDK CDK/BMD shell, scoped to BCM56340, iProc CMIC path). Runs on the NOS rootfs; needs hardware to exercise.
- [ ] Data-plane agent (L2) bring-up (needs hardware: attach → init → ports → L2).
- [ ] L3 + FRR.
- [ ] Kernel forward-port to mainline 6.x.

See the parent folder's `NOS_BUILD_PLAN.md` for the full phased plan and
`FULLY_OPENSOURCE_LINUX_PLAN.md` / `LICENSING.md` for the open-source/licensing
picture.
