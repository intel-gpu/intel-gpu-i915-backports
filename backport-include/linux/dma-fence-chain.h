#ifndef __BACKPORT_LINUX_DMA_FENCE_CHAIN_H
#define __BACKPORT_LINUX_DMA_FENCE_CHAIN_H
#include_next <linux/dma-fence-chain.h>
#include <linux/slab.h>

#ifdef DMA_FENCE_CHAIN_ALLOC_NOT_PRESENT 

/**
 * dma_fence_chain_alloc
 *
 * Returns a new struct dma_fence_chain object or NULL on failure.
 */
static inline struct dma_fence_chain *dma_fence_chain_alloc(void)
{
        return kmalloc(sizeof(struct dma_fence_chain), GFP_KERNEL);
};

/**
 * dma_fence_chain_free
 * @chain: chain node to free
 *
 * Frees up an allocated but not used struct dma_fence_chain object. This
 * doesn't need an RCU grace period since the fence was never initialized nor
 * published. After dma_fence_chain_init() has been called the fence must be
 * released by calling dma_fence_put(), and not through this function.
 */
static inline void dma_fence_chain_free(struct dma_fence_chain *chain)
{
        kfree(chain);
};

#endif 

#endif /* __BACKPORT_LINUX_DMA_FENCE_CHAIN_H */
