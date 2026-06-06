/*
 * edged_dma.h — coherent-DMA hooks for BMD, backed by the OpenBCM kernel BDE.
 * Wired in via -DBMD_SYS_DMA_ALLOC_COHERENT=edged_dma_alloc and
 * -DBMD_SYS_DMA_FREE_COHERENT=edged_dma_free. See edged_dma.c.
 */
#ifndef EDGED_DMA_H
#define EDGED_DMA_H

#include <stddef.h>
#include <cdk/cdk_types.h>

/* BMD_SYS_DMA_ALLOC_COHERENT(dvc, size, &baddr): return uncached virtual addr from
 * the kernel-BDE coherent pool, set *baddr to its chip-usable bus address. NULL on
 * failure (pool not ready / exhausted). */
void *edged_dma_alloc(void *dvc, size_t size, dma_addr_t *baddr);

/* BMD_SYS_DMA_FREE_COHERENT(dvc, size, laddr, baddr): release a block. */
void edged_dma_free(void *dvc, size_t size, void *laddr, dma_addr_t baddr);

#endif /* EDGED_DMA_H */
