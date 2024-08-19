// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include <linux/dma-fence.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "dma_tx.h"

struct dma_tx_fence {
	struct dma_fence base;
	struct dma_async_tx_descriptor *tx;
	spinlock_t lock;
};

static const char *get_driver_name(struct dma_fence *fence)
{
	return "dma-engine";
}

static const char *get_timeline_name(struct dma_fence *fence)
{
	struct dma_tx_fence *f = container_of(fence, typeof(*f), base);

	return dma_chan_name(f->tx->chan);
}

static bool dma_tx_is_signaled(struct dma_fence *fence)
{
	struct dma_tx_fence *f = container_of(fence, typeof(*f), base);

	switch (dma_async_is_tx_complete(f->tx->chan, f->tx->cookie, NULL, NULL)) {
	case DMA_ERROR:
		dma_fence_set_error(&f->base, -EIO);
		fallthrough;
	case DMA_COMPLETE:
		return true;
	default:
		return false;
	}
}

static void dma_tx_release(struct dma_fence *fence)
{
	struct dma_tx_fence *f = container_of(fence, typeof(*f), base);

	dmaengine_desc_free(f->tx);
	dma_fence_free(fence);
}

static void tx_callback(void *param, const struct dmaengine_result *result)
{
	if (unlikely(result->result != DMA_TRANS_NOERROR))
		dma_fence_set_error(param, result->result == DMA_TRANS_ABORTED ? -ECANCELED : -EIO);

	dma_fence_signal(param);
	dma_fence_put(param);
}

static const struct dma_fence_ops notx_ops = {
	.get_driver_name = get_driver_name,
	.get_timeline_name = get_timeline_name,
	.release = dma_tx_release,
};

static const struct dma_fence_ops tx_ops = {
	.get_driver_name = get_driver_name,
	.get_timeline_name = get_timeline_name,
	.signaled = dma_tx_is_signaled,
	.release = dma_tx_release,
};

static struct dma_fence *__tx_create_fence(struct dma_chan *chan)
{
	const struct dma_fence_ops *ops;
	struct dma_tx_fence *f;

	f = kmalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;

	ops = &tx_ops;
	if (dma_has_cap(DMA_COMPLETION_NO_ORDER, chan->device->cap_mask))
		ops = &notx_ops;

	spin_lock_init(&f->lock);
	dma_fence_init(&f->base, ops, &f->lock, 0, 0); /* unordered fences */

	f->tx = NULL;

	return &f->base;
}

static struct dma_fence *
__tx_fence_attach(struct dma_fence *fence, struct dma_async_tx_descriptor *tx)
{
	struct dma_tx_fence *f = container_of(fence, typeof(*f), base);

	f->tx = tx;

	/* DMA_PREP_INTERRUPT */
	dma_fence_get(&f->base);
	tx->callback_result = tx_callback;
	tx->callback_param = f;
	if (tx->flags & DMA_PREP_INTERRUPT)
		__set_bit(DMA_FENCE_FLAG_ENABLE_SIGNAL_BIT, &f->base.flags);

	return fence;
}

struct dma_fence *dma_async_tx_memset(struct dma_chan *chan, dma_addr_t addr, int value, int length)
{
	struct dma_async_tx_descriptor *tx;
	struct dma_fence *fence;

	if (!chan->device->device_prep_dma_memset)
		return NULL;

	fence = __tx_create_fence(chan);
	if (!fence)
		return NULL;

	tx = chan->device->device_prep_dma_memset(chan,
						  addr, value, length,
						  DMA_PREP_INTERRUPT |
						  DMA_CTRL_ACK);
	if (!tx)
		goto free_fence;

	__tx_fence_attach(fence, tx);
	if (dmaengine_submit(tx) < 0)
		goto free_fence;

	chan->device->device_issue_pending(chan);
	return fence;

free_fence:
	kfree(fence);
	return NULL;
}

struct dma_fence *dma_async_tx_memcpy(struct dma_chan *chan, dma_addr_t src, dma_addr_t dst, int length)
{
	struct dma_async_tx_descriptor *tx;
	struct dma_fence *fence;

	if (!chan->device->device_prep_dma_memcpy)
		return NULL;

	fence = __tx_create_fence(chan);
	if (!fence)
		return NULL;

	tx = chan->device->device_prep_dma_memcpy(chan,
						  dst, src, length,
						  DMA_PREP_FENCE | /* posting wait for completion */
						  DMA_PREP_INTERRUPT |
						  DMA_CTRL_ACK);
	if (!tx)
		goto free_fence;

	__tx_fence_attach(fence, tx);
	if (dmaengine_submit(tx) < 0)
		goto free_fence;

	chan->device->device_issue_pending(chan);
	return fence;

free_fence:
	kfree(fence);
	return NULL;
}
