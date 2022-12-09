#include <linux/slab.h>

#ifdef CONFIG_SLAB
#include <linux/slab_def.h>
#endif

#ifdef CONFIG_SLUB
#include <linux/slab_def.h>
#endif

/*
 * Since kmem_cache_get_slabinfo() got introduced in KV5.10.0,
 * added check here. May need to change in future.
 */
#ifdef BPM_KMEM_CACHE_SLABINFO_API_NOT_PRESENT

#ifndef CONFIG_SLOB
#ifdef CONFIG_SLAB
#define get_node  LINUX_I915_BACKPORT(get_node)
static inline struct kmem_cache_node *get_node(struct kmem_cache *s, int node)
{
        return s->node[node];
}
#endif /* CONFIG_SLAB */
#endif /* !CONFIG_SLOB */

/*
 * DEBUG        - 1 for kmem_cache_create() to honour; SLAB_RED_ZONE & SLAB_POISON.
 *                0 for faster, smaller code (especially in the critical paths).
 *
 */
#ifdef CONFIG_DEBUG_SLAB
#define DEBUG	1
#else
#define DEBUG	0
#endif /* CONFIG_DEBUG_SLAB */

#if DEBUG
#define check_irq_on LINUX_I915_BACKPORT(check_irq_on)
static void check_irq_on(void)
{
        BUG_ON(irqs_disabled());
}
#else
#define check_irq_on()  do { } while(0)
#endif /* DEBUG */

#ifdef CONFIG_SLAB

int kmem_cache_get_slabinfo(struct kmem_cache *cachep, struct slabinfo *sinfo)
{
        unsigned long active_objs, num_objs, active_slabs;
        unsigned long total_slabs = 0, free_objs = 0, shared_avail = 0;
        unsigned long free_slabs = 0;
        int node;
        struct kmem_cache_node *n;

        for_each_kmem_cache_node(cachep, node, n) {
                check_irq_on();
                spin_lock_irq(&n->list_lock);

                total_slabs += n->total_slabs;
                free_slabs += n->free_slabs;
                free_objs += n->free_objects;

                if (n->shared)
                        shared_avail += n->shared->avail;

                spin_unlock_irq(&n->list_lock);
        }
        num_objs = total_slabs * cachep->num;
        active_slabs = total_slabs - free_slabs;
        active_objs = num_objs - free_objs;

        sinfo->active_objs = active_objs;
        sinfo->num_objs = num_objs;
        sinfo->active_slabs = active_slabs;
        sinfo->num_slabs = total_slabs;
        sinfo->shared_avail = shared_avail;
        sinfo->limit = cachep->limit;
        sinfo->batchcount = cachep->batchcount;
        sinfo->shared = cachep->shared;
        sinfo->objects_per_slab = cachep->num;
        sinfo->cache_order = cachep->gfporder;

        return 0;
}
EXPORT_SYMBOL_GPL(kmem_cache_get_slabinfo);
#endif /* CONFIG_SLAB */
#endif /* BPM_KMEM_CACHE_SLABINFO_API_NOT_PRESENT */
