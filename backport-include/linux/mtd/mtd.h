#ifndef __BACKPORT_MTD_H
#define __BACKPORT_MTD_H
#include <linux/version.h>
#include <linux/mtd/partitions.h>
#include_next <linux/mtd/mtd.h>

#if LINUX_VERSION_IS_LESS(5,7,0)
struct mtd_part {
        struct mtd_info mtd;
        struct mtd_info *parent;
        uint64_t offset;
        struct list_head list;
};

#define mtd_to_part LINUX_I915_BACKPORT(mtd_to_part)
static inline struct mtd_part *mtd_to_part(struct mtd_info *mtd)
{
        return container_of(mtd, struct mtd_part, mtd);
}

#define mtd_get_master LINUX_I915_BACKPORT(mtd_get_master)
static inline struct mtd_info *mtd_get_master(struct mtd_info *mtd)
{
        struct mtd_part *part=NULL;

        if (!mtd_is_partition(mtd))
                return mtd;

        part = mtd_to_part(mtd);
        return part->parent;
}
#endif /* LINUX_VERSION_IS_LESS */
#endif /* __BACKPORT_MTD_H */
