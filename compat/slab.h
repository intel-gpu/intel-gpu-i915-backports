#ifndef __BACKPORT_MM_SLAB_H
#define __BACKPORT_MM_SLAB_H

#ifdef BPM_FREELIST_ABA_T_NOT_PRESENT

#ifdef CONFIG_64BIT
typedef u128 freelist_full_t;
#else
typedef u64 freelist_full_t;
#endif /*CONFIG_64BIT */

typedef union {
        struct {
                void *freelist;
                unsigned long counter;
        };
        freelist_full_t full;
} freelist_aba_t;
#endif /* BPM_FREELIST_ABA_T_NOT_PRESENT */

#ifdef BPM_FOLIO_ADDRESS_PRESENT
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
#define slab_folio(s)          (_Generic((s),                          \
       const struct slab *:    (const struct folio *)s,                \
       struct slab *:          (struct folio *)s))

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
       void *s_mem;    /* first object */
       unsigned int active;

#elif defined(CONFIG_SLUB)

       union {
               struct list_head slab_list;
               struct rcu_head rcu_head;
#ifdef CONFIG_SLUB_CPU_PARTIAL
               struct {
                       struct slab *next;
                       int slabs;      /* Nr of slabs left */
               };
#endif
       };
       struct kmem_cache *slab_cache;
       /* Double-word boundary */
       void *freelist;         /* first free object */
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
       void *freelist;         /* first free block */
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

#endif /* BPM_FOLIO_ADDRESS_PRESENT */
#endif /* __BACKPORT_SLAB_H */
