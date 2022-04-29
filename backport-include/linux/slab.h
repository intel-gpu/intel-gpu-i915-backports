#ifndef __BACKPORT_SLAB_H
#define __BACKPORT_SLAB_H
#include_next <linux/slab.h>
#include <linux/version.h>

#if LINUX_VERSION_IS_LESS(3,4,0)
/* This backports:
 *
 * commit a8203725dfded5c1f79dca3368a4a273e24b59bb
 * Author: Xi Wang <xi.wang@gmail.com>
 * Date:   Mon Mar 5 15:14:41 2012 -0800
 *
 *	slab: introduce kmalloc_array()
 */

#include <linux/kernel.h> /* for SIZE_MAX */

#define kmalloc_array LINUX_I915_BACKPORT(kmalloc_array)
static inline void *kmalloc_array(size_t n, size_t size, gfp_t flags)
{
	if (size != 0 && n > SIZE_MAX / size)
		return NULL;
	return __kmalloc(n * size, flags);
}
#endif

/*
 * Since kmem_cache_get_slabinfo() got introduced in KV5.10.0,
 * added check here. May need to change in future.
 */
#define slabinfo LINUX_I915_BACKPORT(slabinfo)
struct slabinfo {
	unsigned long active_objs;
	unsigned long num_objs;
	unsigned long active_slabs;
	unsigned long num_slabs;
	unsigned long shared_avail;
	unsigned int limit;
	unsigned int batchcount;
	unsigned int shared;
	unsigned int objects_per_slab;
	unsigned int cache_order;
};

#define kmem_cache_get_slabinfo  LINUX_I915_BACKPORT(kmem_cache_get_slabinfo)

#ifdef CONFIG_SLAB
/*
 * struct array_cache
 *
 * Purpose:
 * - LIFO ordering, to hand out cache-warm objects from _alloc
 * - reduce the number of linked list operations
 * - reduce spinlock operations
 *
 * The limit is stored in the per-cpu structure to reduce the data cache
 * footprint.
 *
 */
#define array_cache LINUX_I915_BACKPORT(array_cache)
struct array_cache {
	unsigned int avail;
	unsigned int limit;
	unsigned int batchcount;
	unsigned int touched;
	void *entry[];	/*
			 * Must have this definition in here for the proper
			 * alignment of array_cache. Also simplifies accessing
			 * the entries.
			 */
};
#endif /* CONFIG_SLAB */

#if defined(CONFIG_SLUB_DEBUG) || defined(CONFIG_SLAB)

int kmem_cache_get_slabinfo(struct kmem_cache *cachep, struct slabinfo *sinfo);

#else
static inline int kmem_cache_get_slabinfo(struct kmem_cache *cachep,
				struct slabinfo *sinfo)
{
	return -EINVAL;
}
#endif

#define kmem_cache_node LINUX_I915_BACKPORT(kmem_cache_node)

#ifndef CONFIG_SLOB

/*
 * The slab lists for all objects.
 */
struct kmem_cache_node {
	spinlock_t list_lock;

#ifdef CONFIG_SLAB
	struct list_head slabs_partial; /* partial list first, better asm code */
	struct list_head slabs_full;
	struct list_head slabs_free;
	unsigned long total_slabs;      /* length of all slab lists */
	unsigned long free_slabs;       /* length of free slab list only */
	unsigned long free_objects;
	unsigned int free_limit;
	unsigned int colour_next;       /* Per-node cache coloring */
	struct array_cache *shared;     /* shared per node */
	struct alien_cache **alien;     /* on other nodes */
	unsigned long next_reap;        /* updated without locking */
	int free_touched;               /* updated without locking */
#endif /* CONFIG_SLAB */

#ifdef CONFIG_SLUB
	unsigned long nr_partial;
	struct list_head partial;
#ifdef CONFIG_SLUB_DEBUG
	atomic_long_t nr_slabs;
	atomic_long_t total_objects;
	struct list_head full;
#endif /* CONFIG_SLUB_DEBUG */
#endif /* CONFIG_SLUB */

};

/*
 * Iterator over all nodes. The body will be executed for each node that has
 * a kmem_cache_node structure allocated (which is true for all online nodes)
 */

#define for_each_kmem_cache_node(__s, __node, __n) \
	for (__node = 0; __node < nr_node_ids; __node++) \
		if ((__n = get_node(__s, __node)))

#endif /* CONFIG_SLOB */

#endif /* __BACKPORT_SLAB_H */
