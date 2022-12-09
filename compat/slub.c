#include <linux/mm.h>
#include <linux/slab.h>

#ifdef CONFIG_SLUB
#include <linux/slub_def.h>
#endif

#ifdef CONFIG_SLAB
#include <linux/slub_def.h>
#endif

/*
 * Since kmem_cache_get_slabinfo() got introduced in KV5.10.0,
 * added check here. May need to change in future.
 */
#ifdef KMEM_CACHE_SLABINFO_API_NOT_PRESENT

#ifndef CONFIG_SLOB
#ifdef CONFIG_SLUB
#define get_node  LINUX_I915_BACKPORT(get_node)
static inline struct kmem_cache_node *get_node(struct kmem_cache *s, int node)
{
        return s->node[node];
}
#endif /* CONFIG_SLUB */
#endif /* !CONFIG_SLOB */

#define OO_SHIFT        16
#define OO_MASK         ((1 << OO_SHIFT) - 1)

#define node_nr_slabs  LINUX_I915_BACKPORT(node_nr_slabs)
#ifdef CONFIG_SLUB_DEBUG

static inline unsigned long node_nr_slabs(struct kmem_cache_node *n)
{
        return atomic_long_read(&n->nr_slabs);
}
#else /* !CONFIG_SLUB_DEBUG */
static inline unsigned long node_nr_slabs(struct kmem_cache_node *n)
{
	return 0;
}
#endif /* CONFIG_SLUB_DEBUG */

#ifdef CONFIG_SLUB_DEBUG
#define count_free  LINUX_I915_BACKPORT(count_free)
#define node_nr_objs  LINUX_I915_BACKPORT(node_nr_objs)
static int count_free(struct page *page)
{
        return page->objects - page->inuse;
}

static inline unsigned long node_nr_objs(struct kmem_cache_node *n)
{
        return atomic_long_read(&n->total_objects);
}
#endif /* CONFIG_SLUB_DEBUG */

#define oo_order  LINUX_I915_BACKPORT(oo_order)
static inline unsigned int oo_order(struct kmem_cache_order_objects x)
{
        return x.x >> OO_SHIFT;
}

#define oo_objects  LINUX_I915_BACKPORT(oo_objects)
static inline unsigned int oo_objects(struct kmem_cache_order_objects x)
{
        return x.x & OO_MASK;
}

#define count_partial  LINUX_I915_BACKPORT(count_partial)

#ifdef CONFIG_SLUB
#if defined(CONFIG_SLUB_DEBUG) || defined(CONFIG_SYSFS)
static unsigned long count_partial(struct kmem_cache_node *n,
                                        int (*get_count)(struct page *))
{
        unsigned long flags;
        unsigned long x = 0;
        struct page *page;

        spin_lock_irqsave(&n->list_lock, flags);
        list_for_each_entry(page, &n->partial, slab_list)
                x += get_count(page);
        spin_unlock_irqrestore(&n->list_lock, flags);
        return x;
}
#endif /* CONFIG_SLUB */
#endif /* CONFIG_SLUB_DEBUG || CONFIG_SYSFS */

#ifdef CONFIG_SLUB_DEBUG
int kmem_cache_get_slabinfo(struct kmem_cache *s, struct slabinfo *sinfo)
{
        unsigned long nr_slabs = 0;
        unsigned long nr_objs = 0;
        unsigned long nr_free = 0;
        int node;
        struct kmem_cache_node *n;

        for_each_kmem_cache_node(s, node, n) {
                nr_slabs += node_nr_slabs(n);
                nr_objs += node_nr_objs(n);
                nr_free += count_partial(n, count_free);
        }

        sinfo->active_objs = nr_objs - nr_free;
        sinfo->num_objs = nr_objs;
        sinfo->active_slabs = nr_slabs;
        sinfo->num_slabs = nr_slabs;
        sinfo->objects_per_slab = oo_objects(s->oo);
        sinfo->cache_order = oo_order(s->oo);

        return 0;
}
EXPORT_SYMBOL_GPL(kmem_cache_get_slabinfo);
#endif /* CONFIG_SLUB_DEBUG */
#endif /* KMEM_CACHE_SLABINFO_API_NOT_PRESENT */
