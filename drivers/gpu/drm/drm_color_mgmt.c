/*
 * Copyright (c) 2016 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include <linux/uaccess.h>

#include <drm/drm_atomic.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_print.h>

#include "drm_crtc_internal.h"

/**
 * DOC: overview
 *
 * Pipe Color management or color space adjustments is supported through a
 * set of 5 properties on the &drm_crtc object. They are set up by calling
 * drm_crtc_enable_color_mgmt().
 *
 * "DEGAMMA_LUT”:
 *	Blob property to set the degamma lookup table (LUT) mapping pixel data
 *	from the framebuffer before it is given to the transformation matrix.
 *	The data is interpreted as an array of &struct drm_color_lut elements.
 *	Hardware might choose not to use the full precision of the LUT elements
 *	nor use all the elements of the LUT (for example the hardware might
 *	choose to interpolate between LUT[0] and LUT[4]).
 *
 *	Setting this to NULL (blob property value set to 0) means a
 *	linear/pass-thru gamma table should be used. This is generally the
 *	driver boot-up state too. Drivers can access this blob through
 *	&drm_crtc_state.degamma_lut.
 *
 * “DEGAMMA_LUT_SIZE”:
 *	Unsinged range property to give the size of the lookup table to be set
 *	on the DEGAMMA_LUT property (the size depends on the underlying
 *	hardware). If drivers support multiple LUT sizes then they should
 *	publish the largest size, and sub-sample smaller sized LUTs (e.g. for
 *	split-gamma modes) appropriately.
 *
 * “CTM”:
 *	Blob property to set the current transformation matrix (CTM) apply to
 *	pixel data after the lookup through the degamma LUT and before the
 *	lookup through the gamma LUT. The data is interpreted as a struct
 *	&drm_color_ctm.
 *
 *	Setting this to NULL (blob property value set to 0) means a
 *	unit/pass-thru matrix should be used. This is generally the driver
 *	boot-up state too. Drivers can access the blob for the color conversion
 *	matrix through &drm_crtc_state.ctm.
 *
 * “GAMMA_LUT”:
 *	Blob property to set the gamma lookup table (LUT) mapping pixel data
 *	after the transformation matrix to data sent to the connector. The
 *	data is interpreted as an array of &struct drm_color_lut elements.
 *	Hardware might choose not to use the full precision of the LUT elements
 *	nor use all the elements of the LUT (for example the hardware might
 *	choose to interpolate between LUT[0] and LUT[4]).
 *
 *	Setting this to NULL (blob property value set to 0) means a
 *	linear/pass-thru gamma table should be used. This is generally the
 *	driver boot-up state too. Drivers can access this blob through
 *	&drm_crtc_state.gamma_lut.
 *
 *	Note that for mostly historical reasons stemming from Xorg heritage,
 *	this is also used to store the color map (also sometimes color lut, CLUT
 *	or color palette) for indexed formats like DRM_FORMAT_C8.
 *
 * “GAMMA_LUT_SIZE”:
 *	Unsigned range property to give the size of the lookup table to be set
 *	on the GAMMA_LUT property (the size depends on the underlying hardware).
 *	If drivers support multiple LUT sizes then they should publish the
 *	largest size, and sub-sample smaller sized LUTs (e.g. for split-gamma
 *	modes) appropriately.
 *
 * There is also support for a legacy gamma table, which is set up by calling
 * drm_mode_crtc_set_gamma_size(). The DRM core will then alias the legacy gamma
 * ramp with "GAMMA_LUT" or, if that is unavailable, "DEGAMMA_LUT".
 *
 * Support for different non RGB color encodings is controlled through
 * &drm_plane specific COLOR_ENCODING and COLOR_RANGE properties. They
 * are set up by calling drm_plane_create_color_properties().
 *
 * "COLOR_ENCODING":
 * 	Optional plane enum property to support different non RGB
 * 	color encodings. The driver can provide a subset of standard
 * 	enum values supported by the DRM plane.
 *
 * "COLOR_RANGE":
 * 	Optional plane enum property to support different non RGB
 * 	color parameter ranges. The driver can provide a subset of
 * 	standard enum values supported by the DRM plane.
 */

/**
 * drm_color_ctm_s31_32_to_qm_n
 *
 * @user_input: input value
 * @m: number of integer bits, only support m <= 32, include the sign-bit
 * @n: number of fractional bits, only support n <= 32
 *
 * Convert and clamp S31.32 sign-magnitude to Qm.n (signed 2's complement).
 * The sign-bit BIT(m+n-1) and above are 0 for positive value and 1 for negative
 * the range of value is [-2^(m-1), 2^(m-1) - 2^-n]
 *
 * For example
 * A Q3.12 format number:
 * - required bit: 3 + 12 = 15bits
 * - range: [-2^2, 2^2 - 2^−15]
 *
 * NOTE: the m can be zero if all bit_precision are used to present fractional
 *       bits like Q0.32
 */
u64 drm_color_ctm_s31_32_to_qm_n(u64 user_input, u32 m, u32 n)
{
	u64 mag = (user_input & ~BIT_ULL(63)) >> (32 - n);
	bool negative = !!(user_input & BIT_ULL(63));
	s64 val;

	WARN_ON(m > 32 || n > 32);

	val = clamp_val(mag, 0, negative ?
				BIT_ULL(n + m - 1) : BIT_ULL(n + m - 1) - 1);

	return negative ? -val : val;
}
EXPORT_SYMBOL(drm_color_ctm_s31_32_to_qm_n);

/**
 * drm_crtc_enable_color_mgmt - enable color management properties
 * @crtc: DRM CRTC
 * @degamma_lut_size: the size of the degamma lut (before CSC)
 * @has_ctm: whether to attach ctm_property for CSC matrix
 * @gamma_lut_size: the size of the gamma lut (after CSC)
 *
 * This function lets the driver enable the color correction
 * properties on a CRTC. This includes 3 degamma, csc and gamma
 * properties that userspace can set and 2 size properties to inform
 * the userspace of the lut sizes. Each of the properties are
 * optional. The gamma and degamma properties are only attached if
 * their size is not 0 and ctm_property is only attached if has_ctm is
 * true.
 */
void drm_crtc_enable_color_mgmt(struct drm_crtc *crtc,
				uint degamma_lut_size,
				bool has_ctm,
				uint gamma_lut_size)
{
	struct drm_device *dev = crtc->dev;
	struct drm_mode_config *config = &dev->mode_config;

	if (degamma_lut_size) {
		drm_object_attach_property(&crtc->base,
					   config->degamma_lut_property, 0);
		drm_object_attach_property(&crtc->base,
					   config->degamma_lut_size_property,
					   degamma_lut_size);
	}

	if (has_ctm)
		drm_object_attach_property(&crtc->base,
					   config->ctm_property, 0);

	if (gamma_lut_size) {
		drm_object_attach_property(&crtc->base,
					   config->gamma_lut_property, 0);
		drm_object_attach_property(&crtc->base,
					   config->gamma_lut_size_property,
					   gamma_lut_size);
	}
}
EXPORT_SYMBOL(drm_crtc_enable_color_mgmt);

void drm_crtc_attach_gamma_degamma_mode_property(struct drm_crtc *crtc,
						 enum lut_type type)
{
	struct drm_property *prop;

	if (type == LUT_TYPE_DEGAMMA)
		prop = crtc->degamma_mode_property;
	else
		prop = crtc->gamma_mode_property;

	if (!prop)
		return;

	drm_object_attach_property(&crtc->base,
				   prop, 0);
}
EXPORT_SYMBOL(drm_crtc_attach_gamma_degamma_mode_property);

int drm_color_create_gamma_mode_property(struct drm_crtc *crtc,
					 int num_values)
{
	struct drm_property *prop;

	prop = drm_property_create(crtc->dev,
				   DRM_MODE_PROP_ENUM,
				   "GAMMA_MODE", num_values);
	if (!prop)
		return -ENOMEM;

	crtc->gamma_mode_property = prop;

	return 0;
}
EXPORT_SYMBOL(drm_color_create_gamma_mode_property);

int drm_color_create_degamma_mode_property(struct drm_crtc *crtc,
					   int num_values)
{
	struct drm_property *prop;

	prop = drm_property_create(crtc->dev,
				   DRM_MODE_PROP_ENUM,
				   "DEGAMMA_MODE", num_values);
	if (!prop)
		return -ENOMEM;

	crtc->degamma_mode_property = prop;

	return 0;
}
EXPORT_SYMBOL(drm_color_create_degamma_mode_property);

int drm_color_add_gamma_degamma_mode_range(struct drm_crtc *crtc,
					   const char *name,
					   const struct drm_color_lut_range *ranges,
					   size_t length, enum lut_type type)
{
	struct drm_property_blob *blob;
	struct drm_property *prop;
	int num_ranges = length / sizeof(ranges[0]);
	int i, ret, num_types_0;

	if (type == LUT_TYPE_DEGAMMA)
		prop = crtc->degamma_mode_property;
	else
		prop = crtc->gamma_mode_property;

	if (!prop)
		return -EINVAL;

	if (length == 0 && name)
		return drm_property_add_enum(prop, 0, name);

	if (WARN_ON(length == 0 || length % sizeof(ranges[0]) != 0))
		return -EINVAL;

	num_types_0 = hweight8(ranges[0].flags & (DRM_MODE_LUT_GAMMA |
			       DRM_MODE_LUT_DEGAMMA));
	if (num_types_0 == 0)
		return -EINVAL;

	for (i = 1; i < num_ranges; i++) {
		int num_types = hweight8(ranges[i].flags & (DRM_MODE_LUT_GAMMA |
					 DRM_MODE_LUT_DEGAMMA));

		/* either all ranges have DEGAMMA|GAMMA or none have it */
		if (num_types_0 != num_types)
			return -EINVAL;
	}

	blob = drm_property_create_blob(crtc->dev, length, ranges);
	if (IS_ERR(blob))
		return PTR_ERR(blob);

	ret = drm_property_add_enum(prop, blob->base.id, name);
	if (ret) {
		drm_property_blob_put(blob);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL(drm_color_add_gamma_degamma_mode_range);

/**
 * drm_mode_crtc_set_gamma_size - set the gamma table size
 * @crtc: CRTC to set the gamma table size for
 * @gamma_size: size of the gamma table
 *
 * Drivers which support gamma tables should set this to the supported gamma
 * table size when initializing the CRTC. Currently the drm core only supports a
 * fixed gamma table size.
 *
 * Returns:
 * Zero on success, negative errno on failure.
 */
int drm_mode_crtc_set_gamma_size(struct drm_crtc *crtc,
				 int gamma_size)
{
	uint16_t *r_base, *g_base, *b_base;
	int i;

	crtc->gamma_size = gamma_size;

	crtc->gamma_store = kcalloc(gamma_size, sizeof(uint16_t) * 3,
				    GFP_KERNEL);
	if (!crtc->gamma_store) {
		crtc->gamma_size = 0;
		return -ENOMEM;
	}

	r_base = crtc->gamma_store;
	g_base = r_base + gamma_size;
	b_base = g_base + gamma_size;
	for (i = 0; i < gamma_size; i++) {
		r_base[i] = i << 8;
		g_base[i] = i << 8;
		b_base[i] = i << 8;
	}


	return 0;
}
EXPORT_SYMBOL(drm_mode_crtc_set_gamma_size);

/**
 * drm_crtc_supports_legacy_gamma - does the crtc support legacy gamma correction table
 * @crtc: CRTC object
 *
 * Returns true/false if the given crtc supports setting the legacy gamma
 * correction table.
 */
static bool drm_crtc_supports_legacy_gamma(struct drm_crtc *crtc)
{
	u32 gamma_id = crtc->dev->mode_config.gamma_lut_property->base.id;
	u32 degamma_id = crtc->dev->mode_config.degamma_lut_property->base.id;

	if (!crtc->gamma_size)
		return false;

	if (crtc->funcs->gamma_set)
		return true;

	return !!(drm_mode_obj_find_prop_id(&crtc->base, gamma_id) ||
		  drm_mode_obj_find_prop_id(&crtc->base, degamma_id));
}

/**
 * drm_crtc_legacy_gamma_set - set the legacy gamma correction table
 * @crtc: CRTC object
 * @red: red correction table
 * @green: green correction table
 * @blue: green correction table
 * @size: size of the tables
 * @ctx: lock acquire context
 *
 * Implements support for legacy gamma correction table for drivers
 * that have set drm_crtc_funcs.gamma_set or that support color management
 * through the DEGAMMA_LUT/GAMMA_LUT properties. See
 * drm_crtc_enable_color_mgmt() and the containing chapter for
 * how the atomic color management and gamma tables work.
 *
 * This function sets the gamma using drm_crtc_funcs.gamma_set if set, or
 * alternatively using crtc color management properties.
 */
static int drm_crtc_legacy_gamma_set(struct drm_crtc *crtc,
				     u16 *red, u16 *green, u16 *blue,
				     u32 size,
				     struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_device *dev = crtc->dev;
	struct drm_atomic_state *state;
	struct drm_crtc_state *crtc_state;
	struct drm_property_blob *blob;
	struct drm_color_lut *blob_data;
	u32 gamma_id = dev->mode_config.gamma_lut_property->base.id;
	u32 degamma_id = dev->mode_config.degamma_lut_property->base.id;
	bool use_gamma_lut;
	int i, ret = 0;
	bool replaced;

	if (crtc->funcs->gamma_set)
		return crtc->funcs->gamma_set(crtc, red, green, blue, size, ctx);

	if (drm_mode_obj_find_prop_id(&crtc->base, gamma_id))
		use_gamma_lut = true;
	else if (drm_mode_obj_find_prop_id(&crtc->base, degamma_id))
		use_gamma_lut = false;
	else
		return -ENODEV;

	state = drm_atomic_state_alloc(crtc->dev);
	if (!state)
		return -ENOMEM;

	blob = drm_property_create_blob(dev,
					sizeof(struct drm_color_lut) * size,
					NULL);
	if (IS_ERR(blob)) {
		ret = PTR_ERR(blob);
		blob = NULL;
		goto fail;
	}

	/* Prepare GAMMA_LUT with the legacy values. */
	blob_data = blob->data;
	for (i = 0; i < size; i++) {
		blob_data[i].red = red[i];
		blob_data[i].green = green[i];
		blob_data[i].blue = blue[i];
	}

	state->acquire_ctx = ctx;
	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state)) {
		ret = PTR_ERR(crtc_state);
		goto fail;
	}

	/* Set GAMMA_LUT and reset DEGAMMA_LUT and CTM */
	replaced = drm_property_replace_blob(&crtc_state->degamma_lut,
					     use_gamma_lut ? NULL : blob);
	replaced |= drm_property_replace_blob(&crtc_state->ctm, NULL);
	replaced |= drm_property_replace_blob(&crtc_state->gamma_lut,
					      use_gamma_lut ? blob : NULL);
	crtc_state->color_mgmt_changed |= replaced;

	ret = drm_atomic_commit(state);

fail:
	drm_atomic_state_put(state);
	drm_property_blob_put(blob);
	return ret;
}

/**
 * drm_mode_gamma_set_ioctl - set the gamma table
 * @dev: DRM device
 * @data: ioctl data
 * @file_priv: DRM file info
 *
 * Set the gamma table of a CRTC to the one passed in by the user. Userspace can
 * inquire the required gamma table size through drm_mode_gamma_get_ioctl.
 *
 * Called by the user via ioctl.
 *
 * Returns:
 * Zero on success, negative errno on failure.
 */
int drm_mode_gamma_set_ioctl(struct drm_device *dev,
			     void *data, struct drm_file *file_priv)
{
	struct drm_mode_crtc_lut *crtc_lut = data;
	struct drm_crtc *crtc;
	void *r_base, *g_base, *b_base;
	int size;
	struct drm_modeset_acquire_ctx ctx;
	int ret = 0;

	if (!drm_core_check_feature(dev, DRIVER_MODESET))
		return -EOPNOTSUPP;

	crtc = drm_crtc_find(dev, file_priv, crtc_lut->crtc_id);
	if (!crtc)
		return -ENOENT;

	if (!drm_crtc_supports_legacy_gamma(crtc))
		return -ENOSYS;

	/* memcpy into gamma store */
	if (crtc_lut->gamma_size != crtc->gamma_size)
		return -EINVAL;

	DRM_MODESET_LOCK_ALL_BEGIN(dev, ctx, 0, ret);

	size = crtc_lut->gamma_size * (sizeof(uint16_t));
	r_base = crtc->gamma_store;
	if (copy_from_user(r_base, (void __user *)(unsigned long)crtc_lut->red, size)) {
		ret = -EFAULT;
		goto out;
	}

	g_base = r_base + size;
	if (copy_from_user(g_base, (void __user *)(unsigned long)crtc_lut->green, size)) {
		ret = -EFAULT;
		goto out;
	}

	b_base = g_base + size;
	if (copy_from_user(b_base, (void __user *)(unsigned long)crtc_lut->blue, size)) {
		ret = -EFAULT;
		goto out;
	}

	ret = drm_crtc_legacy_gamma_set(crtc, r_base, g_base, b_base,
					crtc->gamma_size, &ctx);

out:
	DRM_MODESET_LOCK_ALL_END(dev, ctx, ret);
	return ret;

}

/**
 * drm_mode_gamma_get_ioctl - get the gamma table
 * @dev: DRM device
 * @data: ioctl data
 * @file_priv: DRM file info
 *
 * Copy the current gamma table into the storage provided. This also provides
 * the gamma table size the driver expects, which can be used to size the
 * allocated storage.
 *
 * Called by the user via ioctl.
 *
 * Returns:
 * Zero on success, negative errno on failure.
 */
int drm_mode_gamma_get_ioctl(struct drm_device *dev,
			     void *data, struct drm_file *file_priv)
{
	struct drm_mode_crtc_lut *crtc_lut = data;
	struct drm_crtc *crtc;
	void *r_base, *g_base, *b_base;
	int size;
	int ret = 0;

	if (!drm_core_check_feature(dev, DRIVER_MODESET))
		return -EOPNOTSUPP;

	crtc = drm_crtc_find(dev, file_priv, crtc_lut->crtc_id);
	if (!crtc)
		return -ENOENT;

	/* memcpy into gamma store */
	if (crtc_lut->gamma_size != crtc->gamma_size)
		return -EINVAL;

	drm_modeset_lock(&crtc->mutex, NULL);
	size = crtc_lut->gamma_size * (sizeof(uint16_t));
	r_base = crtc->gamma_store;
	if (copy_to_user((void __user *)(unsigned long)crtc_lut->red, r_base, size)) {
		ret = -EFAULT;
		goto out;
	}

	g_base = r_base + size;
	if (copy_to_user((void __user *)(unsigned long)crtc_lut->green, g_base, size)) {
		ret = -EFAULT;
		goto out;
	}

	b_base = g_base + size;
	if (copy_to_user((void __user *)(unsigned long)crtc_lut->blue, b_base, size)) {
		ret = -EFAULT;
		goto out;
	}
out:
	drm_modeset_unlock(&crtc->mutex);
	return ret;
}

static const char * const color_encoding_name[] = {
	[DRM_COLOR_YCBCR_BT601] = "ITU-R BT.601 YCbCr",
	[DRM_COLOR_YCBCR_BT709] = "ITU-R BT.709 YCbCr",
	[DRM_COLOR_YCBCR_BT2020] = "ITU-R BT.2020 YCbCr",
};

static const char * const color_range_name[] = {
	[DRM_COLOR_YCBCR_FULL_RANGE] = "YCbCr full range",
	[DRM_COLOR_YCBCR_LIMITED_RANGE] = "YCbCr limited range",
};

/**
 * drm_get_color_encoding_name - return a string for color encoding
 * @encoding: color encoding to compute name of
 *
 * In contrast to the other drm_get_*_name functions this one here returns a
 * const pointer and hence is threadsafe.
 */
const char *drm_get_color_encoding_name(enum drm_color_encoding encoding)
{
	if (WARN_ON(encoding >= ARRAY_SIZE(color_encoding_name)))
		return "unknown";

	return color_encoding_name[encoding];
}

/**
 * drm_get_color_range_name - return a string for color range
 * @range: color range to compute name of
 *
 * In contrast to the other drm_get_*_name functions this one here returns a
 * const pointer and hence is threadsafe.
 */
const char *drm_get_color_range_name(enum drm_color_range range)
{
	if (WARN_ON(range >= ARRAY_SIZE(color_range_name)))
		return "unknown";

	return color_range_name[range];
}

/**
 * drm_plane_create_color_properties - color encoding related plane properties
 * @plane: plane object
 * @supported_encodings: bitfield indicating supported color encodings
 * @supported_ranges: bitfileld indicating supported color ranges
 * @default_encoding: default color encoding
 * @default_range: default color range
 *
 * Create and attach plane specific COLOR_ENCODING and COLOR_RANGE
 * properties to @plane. The supported encodings and ranges should
 * be provided in supported_encodings and supported_ranges bitmasks.
 * Each bit set in the bitmask indicates that its number as enum
 * value is supported.
 */
int drm_plane_create_color_properties(struct drm_plane *plane,
				      u32 supported_encodings,
				      u32 supported_ranges,
				      enum drm_color_encoding default_encoding,
				      enum drm_color_range default_range)
{
	struct drm_device *dev = plane->dev;
	struct drm_property *prop;
	struct drm_prop_enum_list enum_list[max_t(int, DRM_COLOR_ENCODING_MAX,
						       DRM_COLOR_RANGE_MAX)];
	int i, len;

	if (WARN_ON(supported_encodings == 0 ||
		    (supported_encodings & -BIT(DRM_COLOR_ENCODING_MAX)) != 0 ||
		    (supported_encodings & BIT(default_encoding)) == 0))
		return -EINVAL;

	if (WARN_ON(supported_ranges == 0 ||
		    (supported_ranges & -BIT(DRM_COLOR_RANGE_MAX)) != 0 ||
		    (supported_ranges & BIT(default_range)) == 0))
		return -EINVAL;

	len = 0;
	for (i = 0; i < DRM_COLOR_ENCODING_MAX; i++) {
		if ((supported_encodings & BIT(i)) == 0)
			continue;

		enum_list[len].type = i;
		enum_list[len].name = color_encoding_name[i];
		len++;
	}

	prop = drm_property_create_enum(dev, 0, "COLOR_ENCODING",
					enum_list, len);
	if (!prop)
		return -ENOMEM;
	plane->color_encoding_property = prop;
	drm_object_attach_property(&plane->base, prop, default_encoding);
	if (plane->state)
		plane->state->color_encoding = default_encoding;

	len = 0;
	for (i = 0; i < DRM_COLOR_RANGE_MAX; i++) {
		if ((supported_ranges & BIT(i)) == 0)
			continue;

		enum_list[len].type = i;
		enum_list[len].name = color_range_name[i];
		len++;
	}

	prop = drm_property_create_enum(dev, 0,	"COLOR_RANGE",
					enum_list, len);
	if (!prop)
		return -ENOMEM;
	plane->color_range_property = prop;
	drm_object_attach_property(&plane->base, prop, default_range);
	if (plane->state)
		plane->state->color_range = default_range;

	return 0;
}
EXPORT_SYMBOL(drm_plane_create_color_properties);

/**
 * DOC: Plane Color Properties
 *
 * Plane Color management or color space adjustments is supported
 * through a set of 5 properties on the &drm_plane object.
 *
 * degamma_mode_property:
 *     Blob property which advertizes the possible degamma modes and
 *     lut ranges supported by the platform. This  allows userspace
 *     to query and get the plane degamma color caps and choose the
 *     appropriate degamma mode and create lut values accordingly
 *
 * degamma_lut_property:
 *	Blob property which allows a userspace to provide LUT values
 *	to apply degamma curve using the h/w plane degamma processing
 *	engine, thereby making the content as linear for further color
 *	processing.
 *
 * ctm_property:
 *	Blob property which allows a userspace to provide CTM coefficients
 *	to do color space conversion or any other enhancement by doing a
 *	matrix multiplication using the h/w CTM processing engine
 *
 * gamma_mode_property:
 *     Blob property which advertizes the possible gamma modes and
 *     lut ranges supported by the platform. This  allows userspace
 *     to query and get the plane gamma color caps and choose the
 *     appropriate gamma mode and create lut values accordingly
 *
 * gamma_lut_property:
 *	Blob property which allows a userspace to provide LUT values
 *	to apply gamma curve using the h/w plane degamma processing
 *	engine, thereby making the content as non-linear.
 *
 */
int drm_plane_create_color_mgmt_properties(struct drm_device *dev,
					   struct drm_plane *plane,
					   int num_values)
{
	struct drm_property *prop;

	prop = drm_property_create(dev, DRM_MODE_PROP_ENUM,
				   "PLANE_DEGAMMA_MODE", num_values);
	if (!prop)
		return -ENOMEM;

	plane->degamma_mode_property = prop;

	prop = drm_property_create(dev, DRM_MODE_PROP_BLOB,
				   "PLANE_DEGAMMA_LUT", 0);
	if (!prop)
		return -ENOMEM;

	plane->degamma_lut_property = prop;

	prop = drm_property_create(dev, DRM_MODE_PROP_BLOB,
				   "PLANE_CTM", 0);
	if (!prop)
		return -ENOMEM;

	plane->ctm_property = prop;

	prop = drm_property_create(dev, DRM_MODE_PROP_ENUM,
				   "PLANE_GAMMA_MODE", num_values);
	if (!prop)
		return -ENOMEM;

	plane->gamma_mode_property = prop;

	prop = drm_property_create(dev, DRM_MODE_PROP_BLOB,
				   "PLANE_GAMMA_LUT", 0);
	if (!prop)
		return -ENOMEM;

	plane->gamma_lut_property = prop;

	return 0;
}
EXPORT_SYMBOL(drm_plane_create_color_mgmt_properties);

void drm_plane_attach_degamma_properties(struct drm_plane *plane)
{
	if (!plane->degamma_mode_property)
		return;

	drm_object_attach_property(&plane->base,
				   plane->degamma_mode_property, 0);

	if (!plane->degamma_lut_property)
		return;

	drm_object_attach_property(&plane->base,
				   plane->degamma_lut_property, 0);
}
EXPORT_SYMBOL(drm_plane_attach_degamma_properties);

void drm_plane_attach_ctm_property(struct drm_plane *plane)
{
	if (!plane->ctm_property)
		return;

	drm_object_attach_property(&plane->base,
				   plane->ctm_property, 0);
}
EXPORT_SYMBOL(drm_plane_attach_ctm_property);

void drm_plane_attach_gamma_properties(struct drm_plane *plane)
{
	if (!plane->gamma_mode_property)
		return;

	drm_object_attach_property(&plane->base,
				   plane->gamma_mode_property, 0);

	if (!plane->gamma_lut_property)
		return;

	drm_object_attach_property(&plane->base,
				   plane->gamma_lut_property, 0);
}
EXPORT_SYMBOL(drm_plane_attach_gamma_properties);

int drm_plane_color_add_gamma_degamma_mode_range(struct drm_plane *plane,
						 const char *name,
						 const struct drm_color_lut_range *ranges,
						 size_t length, enum lut_type type)
{
	struct drm_property_blob *blob;
	struct drm_property *prop = NULL;
	int num_ranges = length / sizeof(ranges[0]);
	int i, ret, num_types_0;

	if (type == LUT_TYPE_DEGAMMA)
		prop = plane->degamma_mode_property;
	else
		prop = plane->gamma_mode_property;

	if (!prop)
		return -EINVAL;

	if (length == 0 && name)
		return drm_property_add_enum(prop, 0, name);

	if (WARN_ON(length == 0 || length % sizeof(ranges[0]) != 0))
		return -EINVAL;
	num_types_0 = hweight8(ranges[0].flags & (DRM_MODE_LUT_GAMMA |
			       DRM_MODE_LUT_DEGAMMA));
	if (num_types_0 == 0)
		return -EINVAL;

	for (i = 1; i < num_ranges; i++) {
		int num_types = hweight8(ranges[i].flags & (DRM_MODE_LUT_GAMMA |
					 DRM_MODE_LUT_DEGAMMA));

		/* either all ranges have DEGAMMA|GAMMA or none have it */
		if (num_types_0 != num_types)
			return -EINVAL;
	}

	blob = drm_property_create_blob(plane->dev, length, ranges);
	if (IS_ERR(blob))
		return PTR_ERR(blob);

	ret = drm_property_add_enum(prop, blob->base.id, name);
	if (ret) {
		drm_property_blob_put(blob);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL(drm_plane_color_add_gamma_degamma_mode_range);

/**
 * drm_color_lut_check - check validity of lookup table
 * @lut: property blob containing LUT to check
 * @tests: bitmask of tests to run
 *
 * Helper to check whether a userspace-provided lookup table is valid and
 * satisfies hardware requirements.  Drivers pass a bitmask indicating which of
 * the tests in &drm_color_lut_tests should be performed.
 *
 * Returns 0 on success, -EINVAL on failure.
 */
int drm_color_lut_check(const struct drm_property_blob *lut, u32 tests)
{
	const struct drm_color_lut *entry;
	int i;

	if (!lut || !tests)
		return 0;

	entry = lut->data;
	for (i = 0; i < drm_color_lut_size(lut); i++) {
		if (tests & DRM_COLOR_LUT_EQUAL_CHANNELS) {
			if (entry[i].red != entry[i].blue ||
			    entry[i].red != entry[i].green) {
				DRM_DEBUG_KMS("All LUT entries must have equal r/g/b\n");
				return -EINVAL;
			}
		}

		if (i > 0 && tests & DRM_COLOR_LUT_NON_DECREASING) {
			if (entry[i].red < entry[i - 1].red ||
			    entry[i].green < entry[i - 1].green ||
			    entry[i].blue < entry[i - 1].blue) {
				DRM_DEBUG_KMS("LUT entries must never decrease.\n");
				return -EINVAL;
			}
		}
	}

	return 0;
}
EXPORT_SYMBOL(drm_color_lut_check);

/**
 * drm_color_lut_ext_check - check validity of extended lookup table
 * @lut: property blob containing extended LUT to check
 * @tests: bitmask of tests to run
 *
 * Helper to check whether a userspace-provided extended lookup table is valid and
 * satisfies hardware requirements.  Drivers pass a bitmask indicating which of
 * the tests in &drm_color_lut_tests should be performed.
 *
 * Returns 0 on success, -EINVAL on failure.
 */
int drm_color_lut_ext_check(const struct drm_property_blob *lut, u32 tests)
{
	const struct drm_color_lut_ext *entry;
	int i;

	if (!lut || !tests)
		return 0;

	entry = lut->data;
	for (i = 0; i < drm_color_lut_ext_size(lut); i++) {
		if (tests & DRM_COLOR_LUT_EQUAL_CHANNELS) {
			if (entry[i].red != entry[i].blue ||
			    entry[i].red != entry[i].green) {
				DRM_DEBUG_KMS("All LUT entries must have equal r/g/b\n");
				return -EINVAL;
			}
		}

		if (i > 0 && tests & DRM_COLOR_LUT_NON_DECREASING) {
			if (entry[i].red < entry[i - 1].red ||
			    entry[i].green < entry[i - 1].green ||
			    entry[i].blue < entry[i - 1].blue) {
				DRM_DEBUG_KMS("LUT entries must never decrease.\n");
				return -EINVAL;
			}
		}
	}

	return 0;
}
EXPORT_SYMBOL(drm_color_lut_ext_check);
