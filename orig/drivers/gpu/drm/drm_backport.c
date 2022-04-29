/*
 * Copyright (C) 2015 Red Hat
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 */

#include <drm/drm_backport.h>
#include <drm/drmP.h>
#include <linux/slab.h>

/*
 * shrinker
 */

#undef shrinker
#undef register_shrinker
#undef unregister_shrinker

static int shrinker2_shrink(struct shrinker *shrinker, struct shrink_control *sc)
{
	struct shrinker2 *s2 = container_of(shrinker, struct shrinker2, compat);
	int count;

	s2->scan_objects(s2, sc);
	count = s2->count_objects(s2, sc);
	shrinker->seeks = s2->seeks;

	return count;
}

int register_shrinker2(struct shrinker2 *s2)
{
	s2->compat.shrink = shrinker2_shrink;
	s2->compat.seeks = s2->seeks;
	register_shrinker(&s2->compat);
	return 0;
}
EXPORT_SYMBOL(register_shrinker2);

void unregister_shrinker2(struct shrinker2 *s2)
{
	unregister_shrinker(&s2->compat);
}
EXPORT_SYMBOL(unregister_shrinker2);

#if IS_ENABLED(CONFIG_SWIOTLB)
#  include <linux/dma-direction.h>
#  include <linux/swiotlb.h>
#endif

unsigned int swiotlb_max_size(void)
{
#if IS_ENABLED(CONFIG_SWIOTLB)
	return rounddown(swiotlb_nr_tbl() << IO_TLB_SHIFT, PAGE_SIZE);
#else
	return 0;
#endif
}
EXPORT_SYMBOL(swiotlb_max_size);

int __init drm_backport_init(void)
{
	return 0;
}

void __exit drm_backport_exit(void)
{
}
