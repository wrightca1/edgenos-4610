/*
 * edged_dma.c — coherent-DMA provider for OpenMDK's BMD on the AS4610-54T.
 *
 * BMD's bmd_dma_alloc_coherent() (used by bcm56340_a0_bmd_rx/tx + xgsm_dma) calls
 * BMD_SYS_DMA_ALLOC_COHERENT(dvc, size, &baddr). The Cortex-A9 is not DMA-coherent,
 * so the buffer must be uncached with a bus address the on-die CMIC DMA engine can
 * reach. We get that from the GPL OpenBCM kernel BDE:
 *
 *   /dev/linux-user-bde   LUBDE_GET_DMA_INFO -> dma_pbase (bus base), size, cpu_pbase
 *   /dev/linux-kernel-bde mmap(offset=cpu_pbase) -> uncached view of the pool
 *                         (kernel maps it pgprot_noncached / dma_mmap_coherent)
 *
 * We then hand out blocks from that single coherent pool. virt = map + off,
 * bus(baddr) = dma_pbase + off. A tiny first-fit allocator with free covers the
 * BMD's DCB + packet-buffer churn (low volume).
 *
 * Build with -DBMD_CONFIG_INCLUDE_DMA=1
 *   -DBMD_SYS_DMA_ALLOC_COHERENT=edged_dma_alloc
 *   -DBMD_SYS_DMA_FREE_COHERENT=edged_dma_free
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>

#include <cdk/cdk_types.h>   /* dma_addr_t (== unsigned int on this build) */

#include "edged_dma.h"

/* ---- linux-user-bde ioctl ABI (must match the GPL module's header) ---------- */
#define LUBDE_MAGIC 'L'
#define LUBDE_GET_DMA_INFO  _IO(LUBDE_MAGIC, 5)
typedef uint32_t bde_kernel_addr_t;   /* 32-bit user + 32-bit kernel on armhf */
typedef struct {
    unsigned int dev, rc, d0, d1, d2, d3;
    bde_kernel_addr_t p0;
    union { unsigned int dw[2]; unsigned char buf[64]; } dx;
} lubde_ioctl_t;

#define DLOG(fmt, ...) fprintf(stderr, "edged-dma: " fmt "\n", ##__VA_ARGS__)

/* ---- pool state ------------------------------------------------------------- */
static int        pool_ready = -1;     /* -1 = uninit, 0 = failed, 1 = ready */
static uint8_t   *pool_virt;           /* mmap base (uncached) */
static uint32_t   pool_dma_base;       /* bus address of pool start */
static uint32_t   pool_size;

#define DMA_ALIGN   64u
#define MAX_BLOCKS  64
static struct { uint32_t off, size; int used; } blk[MAX_BLOCKS];
static int        n_blk;
static uint32_t   hi_water;            /* next fresh offset */

static int
pool_init(void)
{
    int fu, fk;
    lubde_ioctl_t io;
    uint32_t cpu_pbase;

    fu = open("/dev/linux-user-bde", O_RDWR);
    if (fu < 0) { DLOG("open /dev/linux-user-bde: %s", strerror(errno)); return 0; }

    memset(&io, 0, sizeof(io));
    io.dev = 0;
    if (ioctl(fu, LUBDE_GET_DMA_INFO, &io) < 0) {
        DLOG("LUBDE_GET_DMA_INFO: %s", strerror(errno));
        close(fu);
        return 0;
    }
    pool_dma_base = io.d0;          /* bus base the chip DMA uses */
    pool_size     = io.d1;          /* pool size in bytes */
    cpu_pbase     = io.dx.dw[0];    /* cpu phys base = mmap offset */
    close(fu);

    if (pool_size == 0) { DLOG("BDE reports 0 DMA bytes (no dmasize?)"); return 0; }

    fk = open("/dev/linux-kernel-bde", O_RDWR | O_SYNC);
    if (fk < 0) { DLOG("open /dev/linux-kernel-bde: %s", strerror(errno)); return 0; }
    pool_virt = mmap(NULL, pool_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     fk, (off_t)cpu_pbase);
    close(fk);
    if (pool_virt == MAP_FAILED) {
        DLOG("mmap pool (off 0x%x size 0x%x): %s", cpu_pbase, pool_size, strerror(errno));
        pool_virt = NULL;
        return 0;
    }
    DLOG("DMA pool ready: virt %p bus 0x%08x size %u KB (uncached, via linux-bde)",
         (void *)pool_virt, pool_dma_base, pool_size / 1024);
    return 1;
}

void *
edged_dma_alloc(void *dvc, size_t size, dma_addr_t *baddr)
{
    uint32_t need = (uint32_t)((size + DMA_ALIGN - 1) & ~(DMA_ALIGN - 1));
    int i;

    (void)dvc;
    if (pool_ready < 0) {
        pool_ready = pool_init();
    }
    if (pool_ready != 1) {
        return NULL;
    }

    /* reuse a freed block that fits */
    for (i = 0; i < n_blk; i++) {
        if (!blk[i].used && blk[i].size >= need) {
            blk[i].used = 1;
            *baddr = pool_dma_base + blk[i].off;
            return pool_virt + blk[i].off;
        }
    }
    /* otherwise carve fresh space */
    if (hi_water + need > pool_size || n_blk >= MAX_BLOCKS) {
        DLOG("pool exhausted (need %u, hi %u/%u, blocks %d)", need, hi_water, pool_size, n_blk);
        return NULL;
    }
    blk[n_blk].off = hi_water;
    blk[n_blk].size = need;
    blk[n_blk].used = 1;
    *baddr = pool_dma_base + hi_water;
    hi_water += need;
    return pool_virt + blk[n_blk++].off;
}

void
edged_dma_free(void *dvc, size_t size, void *laddr, dma_addr_t baddr)
{
    int i;
    (void)dvc; (void)size; (void)baddr;
    if (pool_ready != 1 || laddr == NULL) {
        return;
    }
    for (i = 0; i < n_blk; i++) {
        if (pool_virt + blk[i].off == (uint8_t *)laddr) {
            blk[i].used = 0;
            return;
        }
    }
}
