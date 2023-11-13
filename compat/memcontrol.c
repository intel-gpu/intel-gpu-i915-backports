// SPDX-License-Identifier: GPL-2.0-or-later
/* memcontrol.c - Memory Controller
 *
 * Copyright IBM Corporation, 2007
 * Author Balbir Singh <balbir@linux.vnet.ibm.com>
 *
 * Copyright 2007 OpenVZ SWsoft Inc
 * Author: Pavel Emelianov <xemul@openvz.org>
 *
 * Memory thresholds
 * Copyright (C) 2009 Nokia Corporation
 * Author: Kirill A. Shutemov
 *
 * Kernel Memory Controller
 * Copyright (C) 2012 Parallels Inc. and Google Inc.
 * Authors: Glauber Costa and Suleiman Souhlal
 *
 * Native page reclaim
 * Charge lifetime sanitation
 * Lockless page tracking & accounting
 * Unified hierarchy configuration model
 * Copyright (C) 2015 Red Hat, Inc., Johannes Weiner
 */

#include<linux/memcontrol.h>

#if IS_ENABLED(CONFIG_MEMCG)

#if defined (BPM_MOD_LRUVEC_STATE_NOT_EXPORTED) || \
	defined (BPM_MOD_MEMCG_LRUVEC_STATE_NOT_PRESENT) || \
	defined (BPM_MOD_LRUVEC_PAGE_STATE_NOT_EXPORTED)

static struct mem_cgroup_per_node *
parent_nodeinfo(struct mem_cgroup_per_node *pn, int nid)
{
        struct mem_cgroup *parent;

        parent = parent_mem_cgroup(pn->memcg);
        if (!parent)
                return NULL;
        return mem_cgroup_nodeinfo(parent, nid);
}
#endif

#if defined (BPM_MOD_LRUVEC_STATE_NOT_EXPORTED) || \
	defined (BPM_MOD_LRUVEC_PAGE_STATE_NOT_EXPORTED)
/**
 * __mod_memcg_state - update cgroup memory statistics
 * @memcg: the memory cgroup
 * @idx: the stat item - can be enum memcg_stat_item or enum node_stat_item
 * @val: delta to add to the counter, can be negative
 */
void __mod_memcg_state(struct mem_cgroup *memcg, int idx, int val)
{
        long x, threshold = MEMCG_CHARGE_BATCH;

        if (mem_cgroup_disabled())
                return;

        if (memcg_stat_item_in_bytes(idx))
                threshold <<= PAGE_SHIFT;

        x = val + __this_cpu_read(memcg->vmstats_percpu->stat[idx]);
        if (unlikely(abs(x) > threshold)) {
                struct mem_cgroup *mi;

                /*
                 * Batch local counters to keep them in sync with
                 * the hierarchical ones.
                 */
                __this_cpu_add(memcg->vmstats_local->stat[idx], x);
                for (mi = memcg; mi; mi = parent_mem_cgroup(mi))
                        atomic_long_add(x, &mi->vmstats[idx]);
                x = 0;
        }
        __this_cpu_write(memcg->vmstats_percpu->stat[idx], x);
}

void __mod_memcg_lruvec_state(struct lruvec *lruvec, enum node_stat_item idx,
                              int val)
{
        struct mem_cgroup_per_node *pn;
        struct mem_cgroup *memcg;
        long x, threshold = MEMCG_CHARGE_BATCH;

        pn = container_of(lruvec, struct mem_cgroup_per_node, lruvec);
        memcg = pn->memcg;

        /* Update memcg */
        __mod_memcg_state(memcg, idx, val);

        /* Update lruvec */
        __this_cpu_add(pn->lruvec_stat_local->count[idx], val);

        if (vmstat_item_in_bytes(idx))
                threshold <<= PAGE_SHIFT;

        x = val + __this_cpu_read(pn->lruvec_stat_cpu->count[idx]);
        if (unlikely(abs(x) > threshold)) {
                pg_data_t *pgdat = lruvec_pgdat(lruvec);
                struct mem_cgroup_per_node *pi;

                for (pi = pn; pi; pi = parent_nodeinfo(pi, pgdat->node_id))
                        atomic_long_add(x, &pi->lruvec_stat[idx]);
                x = 0;
        }
        __this_cpu_write(pn->lruvec_stat_cpu->count[idx], x);
}

#endif

#ifdef BPM_MOD_LRUVEC_STATE_NOT_EXPORTED
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
        /* Update node */
        __mod_node_page_state(lruvec_pgdat(lruvec), idx, val);

        /* Update memcg and lruvec */
        if (!mem_cgroup_disabled())
                __mod_memcg_lruvec_state(lruvec, idx, val);
}
EXPORT_SYMBOL_GPL(__mod_lruvec_state);

#elif defined BPM_MOD_MEMCG_LRUVEC_STATE_NOT_PRESENT
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
                 * Batch local counters to keep them in sync with
                 * the hierarchical ones.
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
EXPORT_SYMBOL_GPL(__mod_lruvec_state);

#endif

#ifdef BPM_MOD_LRUVEC_PAGE_STATE_NOT_EXPORTED
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
        /* Update node */
        __mod_node_page_state(lruvec_pgdat(lruvec), idx, val);

        /* Update memcg and lruvec */
        if (!mem_cgroup_disabled())
                __mod_memcg_lruvec_state(lruvec, idx, val);
}

void __mod_lruvec_page_state(struct page *page, enum node_stat_item idx,
                             int val)
{
        struct page *head = compound_head(page); /* rmap on tail pages */
        struct mem_cgroup *memcg;
        pg_data_t *pgdat = page_pgdat(page);
        struct lruvec *lruvec;

        rcu_read_lock();
        memcg = page_memcg(head);
        /* Untracked pages have no memcg, no lruvec. Update only the node */
        if (!memcg) {
                rcu_read_unlock();
                __mod_node_page_state(pgdat, idx, val);
                return;
        }

        lruvec = mem_cgroup_lruvec(memcg, pgdat);
        __mod_lruvec_state(lruvec, idx, val);
        rcu_read_unlock();
}
EXPORT_SYMBOL(__mod_lruvec_page_state);
#endif

#endif /* IS_ENABLED(CONFIG_MEMCG) */
