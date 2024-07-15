#ifndef __BACKPORT_DMA_RESV_H
#define __BACKPORT_DMA_RESV_H

#include_next <linux/dma-resv.h>

#ifdef BPM_DMA_RESV_EXCL_FENCE_NOT_PRESENT
#define DMA_RESV_LIST_MASK      0x3

/* 
 * Note: Below functions and structure are local to dma-resv.c file, 
 * backporting it locally. Structural changes in dma-resv in newer 
 * kernel may lead to issues, verify the structural changes if any 
 * related issues arises.
 */

struct dma_resv_list {
        struct rcu_head rcu;
        u32 num_fences, max_fences;
        struct dma_fence __rcu *table[];
};

/* Extract the fence and usage flags from an RCU protected entry in the list. */
static inline void dma_resv_list_entry(struct dma_resv_list *list, unsigned int index,
                struct dma_resv *resv, struct dma_fence **fence,
                enum dma_resv_usage *usage)
{
    long tmp;

    tmp = (long)rcu_dereference_check(list->table[index],
                      resv ? dma_resv_held(resv) : true);
    *fence = (struct dma_fence *)(tmp & ~DMA_RESV_LIST_MASK);
    if (usage)
        *usage = tmp & DMA_RESV_LIST_MASK;
}

/* Dereference the fences while ensuring RCU rules */
static inline struct dma_resv_list *dma_resv_fences_list(struct dma_resv *obj)
{
    return rcu_dereference_check(obj->fences, dma_resv_held(obj));
}
#endif

#ifdef BPM_DMA_RESV_RESERVE_SHARED_NOT_PRESENT
#define dma_resv_reserve_shared dma_resv_reserve_fences
#endif

#ifdef BPM_DMA_RESV_TEST_SIGNALED_BOOLEAN_ARG_NOT_PRESENT
bool backport_dma_resv_test_signaled(struct dma_resv *obj, bool test_all);
#define dma_resv_test_signaled backport_dma_resv_test_signaled
#endif

#ifdef BPM_DMA_RESV_EXCL_UNLOCKED_NOT_PRESENT

/**
 * dma_resv_get_excl_unlocked - get the reservation object's
 * exclusive fence, without lock held.
 * @obj: the reservation object
 *
 * If there is an exclusive fence, this atomically increments it's
 * reference count and returns it.
 *
 * RETURNS
 * The exclusive fence or NULL if none
 */
#define dma_resv_get_excl_unlocked LINUX_I915_BACKPORT(dma_resv_get_excl_unlocked)
static inline struct dma_fence *
dma_resv_get_excl_unlocked(struct dma_resv *obj)
{
        struct dma_fence *fence;

        if (!rcu_access_pointer(obj->fence_excl))
                return NULL;

        rcu_read_lock();
        fence = dma_fence_get_rcu_safe(&obj->fence_excl);
        rcu_read_unlock();

        return fence;
}
#endif /* BPM_DMA_RESV_EXCL_UNLOCKED_NOT_PRESENT */

#endif /* __BACKPORT_DMA_RESV_H */
