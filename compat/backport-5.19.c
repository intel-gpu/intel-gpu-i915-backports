// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 1994-1999  Linus Torvalds
 */

#include <linux/fs.h>
#include <linux/mm_types.h>
#include <drm/drm_dp_helper.h>

#ifdef BPM_PAGECACHE_WRITE_BEGIN_AND_END_NOT_PRESENT

int pagecache_write_begin(struct file *file, struct address_space *mapping,
                                loff_t pos, unsigned len, unsigned flags,
                                struct page **pagep, void **fsdata)
{
        const struct address_space_operations *aops = mapping->a_ops;

        return aops->write_begin(file, mapping, pos, len,
                                 pagep, fsdata);
}
EXPORT_SYMBOL(pagecache_write_begin);

int pagecache_write_end(struct file *file, struct address_space *mapping,
                                loff_t pos, unsigned len, unsigned copied,
                                struct page *page, void *fsdata)
{
        const struct address_space_operations *aops = mapping->a_ops;

        return aops->write_end(file, mapping, pos, len, copied, page, fsdata);
}
EXPORT_SYMBOL(pagecache_write_end);

#endif

#ifdef BPM_DP_READ_LTTPR_CAPS_DPCD_ARG_NOT_PRESENT

/**
 * drm_dp_read_lttpr_common_caps - read the LTTPR common capabilities
 * @aux: DisplayPort AUX channel
 * @caps: buffer to return the capability info in
 *
 * Read capabilities common to all LTTPRs.
 *
 * Returns 0 on success or a negative error code on failure.
 */
int drm_dp_read_lttpr_common_caps(struct drm_dp_aux *aux,
                                  u8 caps[DP_LTTPR_COMMON_CAP_SIZE])
{
        int ret;

        ret = drm_dp_dpcd_read(aux,
                               DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV,
                               caps, DP_LTTPR_COMMON_CAP_SIZE);
        if (ret < 0)
                return ret;

        WARN_ON(ret != DP_LTTPR_COMMON_CAP_SIZE);

        return 0;
}
EXPORT_SYMBOL(drm_dp_read_lttpr_common_caps);

/**
 * drm_dp_read_lttpr_phy_caps - read the capabilities for a given LTTPR PHY
 * @aux: DisplayPort AUX channel
 * @dp_phy: LTTPR PHY to read the capabilities for
 * @caps: buffer to return the capability info in
 *
 * Read the capabilities for the given LTTPR PHY.
 *
 * Returns 0 on success or a negative error code on failure.
 */
int drm_dp_read_lttpr_phy_caps(struct drm_dp_aux *aux,
                               enum drm_dp_phy dp_phy,
                               u8 caps[DP_LTTPR_PHY_CAP_SIZE])
{
        int ret;

        ret = drm_dp_dpcd_read(aux,
                               DP_TRAINING_AUX_RD_INTERVAL_PHY_REPEATER(dp_phy),
                               caps, DP_LTTPR_PHY_CAP_SIZE);
        if (ret < 0)
                return ret;

        WARN_ON(ret != DP_LTTPR_PHY_CAP_SIZE);

        return 0;
}
EXPORT_SYMBOL(drm_dp_read_lttpr_phy_caps);

#endif
