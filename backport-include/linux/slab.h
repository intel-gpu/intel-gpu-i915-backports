#ifndef __BACKPORT_SLAB_H
#define __BACKPORT_SLAB_H
#include_next <linux/slab.h>
#include <linux/version.h>
#include <linux/sched/mm.h>

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
	unsigned long total_slabs;	/* length of all slab lists */
	unsigned long free_slabs;	/* length of free slab list only */
	unsigned long free_objects;
	unsigned int free_limit;
	unsigned int colour_next;	/* Per-node cache coloring */
	struct array_cache *shared;	/* shared per node */
	struct alien_cache **alien;	/* on other nodes */
	unsigned long next_reap;	/* updated without locking */
	int free_touched;		/* updated without locking */
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

#ifdef FOLIO_ADDRESS_PRESENT

#if defined(CONFIG_SLUB) || defined(CONFIG_SLAB)

/**
 * slab_folio - The folio allocated for a slab
 * @slab: The slab.
 *
 * Slabs are allocated as folios that contain the individual objects and are
 * using some fields in the first struct page of the folio - those fields are
 * now accessed by struct slab. It is occasionally necessary to convert back to
 * a folio in order to communicate with the rest of the mm.  Please use this
 * helper function instead of casting yourself, as the implementation may change
 * in the future.
 */
#define slab_folio(s)		(_Generic((s),				\
	const struct slab *:	(const struct folio *)s,		\
	struct slab *:		(struct folio *)s))

#define slab_address LINUX_I915_BACKPORT(slab_address)
static inline void *slab_address(const struct slab *slab)
{
	return folio_address(slab_folio(slab));
}
#endif /* defined(CONFIG_SLUB) || defined(CONFIG_SLAB) */

/*
 * Internal slab definitions
 */

/* Reuses the bits in struct page */
struct slab {
	unsigned long __page_flags;

#if defined(CONFIG_SLAB)

	union {
		struct list_head slab_list;
		struct rcu_head rcu_head;
	};
	struct kmem_cache *slab_cache;
	void *freelist; /* array of free object indexes */
	void *s_mem;	/* first object */
	unsigned int active;

#elif defined(CONFIG_SLUB)

	union {
		struct list_head slab_list;
		struct rcu_head rcu_head;
#ifdef CONFIG_SLUB_CPU_PARTIAL
		struct {
			struct slab *next;
			int slabs;	/* Nr of slabs left */
		};
#endif
	};
	struct kmem_cache *slab_cache;
	/* Double-word boundary */
	void *freelist;		/* first free object */
	union {
		unsigned long counters;
		struct {
			unsigned inuse:16;
			unsigned objects:15;
			unsigned frozen:1;
		};
	};
	unsigned int __unused;

#elif defined(CONFIG_SLOB)

       struct list_head slab_list;
	void *__unused_1;
	void *freelist;		/* first free block */
	long units;
	unsigned int __unused_2;

#else
#error "Unexpected slab allocator configured"
#endif

	atomic_t __page_refcount;
#ifdef CONFIG_MEMCG
	unsigned long memcg_data;
#endif
};

#endif /* FOLIO_ADDRESS_PRESENT */
#endif /* __BACKPORT_SLAB_H */
