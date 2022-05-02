#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/swap.h>
#include <linux/pagemap.h>
#include <linux/memcontrol.h>

struct mem_cgroup *root_mem_cgroup __read_mostly;

void mem_cgroup_update_lru_size(struct lruvec *lruvec, enum lru_list lru,
                                int zid, int nr_pages)
{
        struct mem_cgroup_per_node *mz;
        unsigned long *lru_size;
        long size;

        if (mem_cgroup_disabled())
                return;

        mz = container_of(lruvec, struct mem_cgroup_per_node, lruvec);
        lru_size = &mz->lru_zone_size[zid][lru];

        if (nr_pages < 0)
                *lru_size += nr_pages;

        size = *lru_size;
        if (WARN_ONCE(size < 0,
                "%s(%p, %d, %d): lru_size %ld\n",
                __func__, lruvec, lru, nr_pages, size)) {
                VM_BUG_ON(1);
                *lru_size = 0;
        }

        if (nr_pages > 0)
                *lru_size += nr_pages;
}



#if RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(8,5)
static struct mem_cgroup_per_node *
mem_cgroup_page_nodeinfo(struct mem_cgroup *memcg, struct page *page)
{
        int nid = page_to_nid(page);

        return memcg->nodeinfo[nid];
}

struct lruvec *mem_cgroup_page_lruvec(struct page *page, struct pglist_data *pgdat)
{
        struct mem_cgroup_per_node *mz;
        struct mem_cgroup *memcg;
        struct lruvec *lruvec;

        if (mem_cgroup_disabled()) {
                lruvec = &pgdat->__lruvec;
                goto out;
        }

        memcg = page_memcg(page);
        /*
         * Swapcache readahead pages are added to the LRU - and
         * possibly migrated - before they are charged.
         */
        if (!memcg)
                memcg = root_mem_cgroup;

        mz = mem_cgroup_page_nodeinfo(memcg, page);
        lruvec = &mz->lruvec;
out:
        /*
         * Since a node can be onlined after the mem_cgroup was created,
         * we have to be prepared to initialize lruvec->zone here;
         * and if offlined then reonlined, we need to reinitialize it.
         */
        if (unlikely(lruvec->pgdat != pgdat))
                lruvec->pgdat = pgdat;
        return lruvec;
}
#endif /* RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(8,5) */

static struct mem_cgroup_per_node *
parent_nodeinfo(struct mem_cgroup_per_node *pn, int nid)
{
        struct mem_cgroup *parent;

        parent = parent_mem_cgroup(pn->memcg);
        if (!parent)
                return NULL;
        return mem_cgroup_nodeinfo(parent, nid);
}

/**
 * __mod_memcg_state - update cgroup memory statistics
 * @memcg: the memory cgroup
 * @idx: the stat item - can be enum memcg_stat_item or enum node_stat_item
 * @val: delta to add to the counter, can be negative
 */
void __mod_memcg_state(struct mem_cgroup *memcg, int idx, int val)
{
        long x;

        if (mem_cgroup_disabled())
                return;

        x = val + __this_cpu_read(memcg->vmstats_percpu->stat[idx]);
        if (unlikely(abs(x) > MEMCG_CHARGE_BATCH)) {
                struct mem_cgroup *mi;

/*
 *                  * Batch local counters to keep them in sync with
 *                                   * the hierarchical ones.
 */
                __this_cpu_add(memcg->vmstats_local->stat[idx], x);
                for (mi = memcg; mi; mi = parent_mem_cgroup(mi))
                        atomic_long_add(x, &mi->vmstats[idx]);
                x = 0;
        }
        __this_cpu_write(memcg->vmstats_percpu->stat[idx], x);
}

/**
 * __mod_lruvec_state - update lruvec memory statistics
 * @lruvec: the lruvec
 * @idx: the stat item
 * @val: delta to add to the counter, can be negative
 *
 * The lruvec is the intersection of the NUMA node and a cgroup. This
 * function updates the all three counters that are affected by a
 * change of state at this level: per-node, per-cgroup, per-lruvec.
 */
void __mod_lruvec_state(struct lruvec *lruvec, enum node_stat_item idx,
                        int val)
{
        pg_data_t *pgdat = lruvec_pgdat(lruvec);
        struct mem_cgroup_per_node *pn;
        struct mem_cgroup *memcg;
        long x;

        /* Update node */
        __mod_node_page_state(pgdat, idx, val);

        if (mem_cgroup_disabled())
                return;

        pn = container_of(lruvec, struct mem_cgroup_per_node, lruvec);
        memcg = pn->memcg;

        /* Update memcg */
        __mod_memcg_state(memcg, idx, val);

        /* Update lruvec */
        __this_cpu_add(pn->lruvec_stat_local->count[idx], val);

        x = val + __this_cpu_read(pn->lruvec_stat_cpu->count[idx]);
        if (unlikely(abs(x) > MEMCG_CHARGE_BATCH)) {
                struct mem_cgroup_per_node *pi;

                for (pi = pn; pi; pi = parent_nodeinfo(pi, pgdat->node_id))
                        atomic_long_add(x, &pi->lruvec_stat[idx]);
                x = 0;
        }
        __this_cpu_write(pn->lruvec_stat_cpu->count[idx], x);
}


/*
 * page_evictable - test whether a page is evictable
 * @page: the page to test
 *
 * Test whether page is evictable--i.e., should be placed on active/inactive
 * lists vs unevictable list.
 *
 * Reasons page might not be evictable:
 * (1) page's mapping marked unevictable
 * (2) page is part of an mlocked VMA
 *
 */
int page_evictable(struct page *page)
{
        int ret;

        /* Prevent address_space of inode and swap cache from being freed */
        rcu_read_lock();
        ret = !mapping_unevictable(page_mapping(page)) && !PageMlocked(page);
        rcu_read_unlock();
        return ret;
}

#if RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(8,5)
struct lruvec *lock_page_lruvec_irq(struct page *page)
{
        struct lruvec *lruvec;
        struct pglist_data *pgdat = page_pgdat(page);

        lruvec = mem_cgroup_page_lruvec(page, pgdat);
        spin_lock_irq(&lruvec->lru_lock);

        lruvec_memcg_debug(lruvec, page);

        return lruvec;
}
#endif /* RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(8,5) */

/**
 * check_move_unevictable_pages - check pages for evictability and move to
 * appropriate zone lru list
 * @pvec: pagevec with lru pages to check
 *
 * Checks pages for evictability, if an evictable page is in the unevictable
 * lru list, moves it to the appropriate evictable lru list. This function
 * should be only used for lru pages.
 */

#if RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(8,5)
void check_move_unevictable_pages(struct page **pages, int nr_pages)
{
        struct lruvec *lruvec;
        struct pglist_data *pgdat = NULL;
        int pgscanned = 0;
        int pgrescued = 0;
        int i;

        for (i = 0; i < nr_pages; i++) {
                struct page *page = pages[i];
                struct pglist_data *pagepgdat = page_pgdat(page);

                pgscanned++;
                if (pagepgdat != pgdat) {
                        if (pgdat)
                                spin_unlock_irq(&pgdat->lru_lock);
                        pgdat = pagepgdat;
                        spin_lock_irq(&pgdat->lru_lock);
                }
                lruvec = mem_cgroup_page_lruvec(page, pgdat);

                if (!PageLRU(page) || !PageUnevictable(page))
                        continue;

                if (page_evictable(page)) {
                        enum lru_list lru = page_lru_base_type(page);

                        VM_BUG_ON_PAGE(PageActive(page), page);
                        ClearPageUnevictable(page);
                        del_page_from_lru_list(page, lruvec, LRU_UNEVICTABLE);
                        add_page_to_lru_list(page, lruvec, lru);
                        pgrescued++;
                }
        }

        if (pgdat) {
                __count_vm_events(UNEVICTABLE_PGRESCUED, pgrescued);
                __count_vm_events(UNEVICTABLE_PGSCANNED, pgscanned);
                spin_unlock_irq(&pgdat->lru_lock);
        }
}
#else
void check_move_unevictable_pages(struct pagevec *pvec,int nr_pages)
{
        struct lruvec *lruvec = NULL;
        int pgscanned = 0;
        int pgrescued = 0;
        int i;

        for (i = 0; i < pvec->nr; i++) {
                struct page *page = pvec->pages[i];
                int nr_pages;

                if (PageTransTail(page))
                        continue;

                nr_pages = thp_nr_pages(page);
                pgscanned += nr_pages;

                /* block memcg migration during page moving between lru */
                if (!TestClearPageLRU(page))
                        continue;

                lruvec = relock_page_lruvec_irq(page, lruvec);
                if (page_evictable(page) && PageUnevictable(page)) {
                        enum lru_list lru = page_lru_base_type(page);

                        VM_BUG_ON_PAGE(PageActive(page), page);
                        ClearPageUnevictable(page);
                        del_page_from_lru_list(page, lruvec, LRU_UNEVICTABLE);
                        add_page_to_lru_list(page, lruvec, lru);
                        pgrescued += nr_pages;
                }
                SetPageLRU(page);
        }
       if (lruvec) {
                __count_vm_events(UNEVICTABLE_PGRESCUED, pgrescued);
                __count_vm_events(UNEVICTABLE_PGSCANNED, pgscanned);
                unlock_page_lruvec_irq(lruvec);
        } else if (pgscanned) {
                count_vm_events(UNEVICTABLE_PGSCANNED, pgscanned);
        }
}
#endif /* RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(8,5) */
EXPORT_SYMBOL_GPL(check_move_unevictable_pages);

