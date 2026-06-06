# AS4610 CPU punt path (RX/TX DMA) — de-risked plan

Goal: give the CPU a packet path to/from the BCM56340 so ARP/control frames reach
edged and routed frames can be injected — the foundation for dynamic L3 (ARP →
EGR/ING_L3_NEXT_HOP) on top of the existing hardware L3 forwarding (edged_l3.c).

## What we found (all confirmed)
1. **OpenMDK already has the DMA engine.** `bcm56340_a0_bmd_rx/tx` + `xgsm_dma.c`
   drive the CMICm CMC0 channels (CMIC_CMC_DMA_CTRL/DESC/STAT), RX/TX DCB (64-byte,
   16-word), CPU-port enable via EPC_LINK_BMAP (CMIC_PORT = port 0). Gated behind
   `BMD_CONFIG_INCLUDE_DMA=1`; the only missing piece is a coherent-DMA provider
   (`BMD_SYS_DMA_ALLOC_COHERENT` / `_FREE_COHERENT`).
2. **Cortex-A9 DMA is not cache-coherent** → the DMA pool must be uncached. That
   needs a kernel allocator (dma_alloc_coherent); pure cached userspace is unsafe.
3. **The provider = OpenBCM's GPL linux-kernel-bde.** Located at
   `live-investigation/sdk-ref/sdk-6.5.16/src/gpl-modules/systems/bde/linux/kernel/`
   (`linux-kernel-bde.c`, `linux_dma.c`, `mpool.c`, shared `shbde_*.c`).
   `MODULE_LICENSE("GPL")`, GPLv2 headers — genuinely open, usable + modifiable.
   (NOT the proprietary `bcm-core.c`, and NOT the deleted ariavie SDK.)
4. **It has native on-die iProc CMICd support** — `IPROC_CMICD_BASE 0x48000000`
   (our exact CMIC base), `IPROC_CMICD_SIZE 0x40000`, `IPROC_CMICD_INT 194`, probed
   via device-tree `compatible = "brcm,iproc-cmicd"` in `iproc_cmicd_probe()`
   (sets BDE_AXI_DEV_TYPE | BDE_SWITCH_DEV_TYPE). Built with `IPROC_CMICD` defined
   (Makefile.linux-iproc).
5. **The box DT already has the node**: `/proc/device-tree/axi/iproc_cmicd@48000000`
   with compatible containing `iproc-cmicd` → the module auto-probes the 56340 at
   load. No DTB overlay needed.
6. **Matching kernel tree exists for the cross-build**:
   `OpenNetworkLinux/packages/base/armhf/kernels/kernel-4.14-lts-armhf-iproc-all/
   builds/stretch/linux-4.14.151/` (with .config + Module.symvers). Box vermagic:
   `4.14.151-OpenNetworkLinux-armhf SMP preempt mod_unload ARMv7 p2v8`.

## Steps
- [ ] Cross-build linux-kernel-bde.ko (+ linux_dma/mpool/shbde) for armhf against the
      ONL 4.14.151 tree, `IPROC_CMICD` defined. (Builder image has arm-linux-gnueabihf.)
- [ ] insmod on box → creates /dev/linux-kernel-bde, probes 56340 (dev 0xb340).
- [ ] Wire edged BMD_SYS_DMA_ALLOC_COHERENT/_FREE_COHERENT to the BDE: open
      /dev/linux-kernel-bde, ioctl for DMA pool info, mmap pool, virt<->bus xlate.
      Rebuild edged with BMD_CONFIG_INCLUDE_DMA=1, call bmd_xgsm_dma_init.
- [ ] Verify: bmd_tx a crafted frame out a port; bmd_rx_poll catches an ingress
      frame punted to CPU (trap rule / copy-to-cpu). Then build ARP on top.

## Note
The box has no toolchain/headers; module is cross-built on the host and shipped as
a .ko. linux-user-bde is also present (gpl-modules) if we prefer the user-BDE mmap
path over a custom hook — both are GPLv2.
