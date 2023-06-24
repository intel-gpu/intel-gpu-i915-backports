/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __BACKPORT_DRM_MM_H
#define __BACKPORT_DRM_MM_H

#include_next <drm/drm_mm.h>

#ifdef DRM_MM_FOR_EACH_NODE_IN_RANGE_SAFE_NOT_PRESENT

/*
 * drm_mm_for_each_node_in_range_safe - iterator to walk over a range of
 * allocated nodes
 * @node__: drm_mm_node structure to assign to in each iteration step
 * @next__: &struct drm_mm_node to store the next step
 * @mm__: drm_mm allocator to walk
 * @start__: starting offset, the first node will overlap this
 * @end__: ending offset, the last node will start before this (but may overlap)
 *
 * This iterator walks over all nodes in the range allocator that lie
 * between @start and @end. It is implemented similarly to list_for_each_safe(),
 * so save against removal of elements.
 */
#define drm_mm_for_each_node_in_range_safe(node__, next__, mm__, start__, end__)        \
        for (node__ = __drm_mm_interval_first((mm__), (start__), (end__)-1), \
                next__ = list_next_entry(node__, node_list); \
             node__->start < (end__);                                   \
             node__ = next__, next__ = list_next_entry(next__, node_list))

#endif
#endif /* __BACKPORT_DRM_MM_H */

