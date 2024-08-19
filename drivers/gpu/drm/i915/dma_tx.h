/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __DMA_TX_H__
#define __DMA_TX_H__

#include <linux/types.h>

struct dma_chan;
struct dma_fence;

struct dma_fence *dma_async_tx_memset(struct dma_chan *chan, dma_addr_t addr, int value, int length);
struct dma_fence *dma_async_tx_memcpy(struct dma_chan *chan, dma_addr_t src, dma_addr_t dst, int length);

#endif /* __DMA_TX_H__ */
