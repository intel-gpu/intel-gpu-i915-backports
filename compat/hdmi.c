#include <linux/version.h> 
#include <linux/hdmi.h>

#if LINUX_VERSION_IS_LESS(5,8,0) && \
	(!defined(RHEL_RELEASE_CODE) || (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(8,4)))

#define hdmi_drm_infoframe_unpack_only  LINUX_I915_BACKPORT(hdmi_drm_infoframe_unpack_only)
/**
 * hdmi_drm_infoframe_unpack_only() - unpack binary buffer of CTA-861-G DRM
 *                                    infoframe DataBytes to a HDMI DRM
 *                                    infoframe
 * @frame: HDMI DRM infoframe
 * @buffer: source buffer
 * @size: size of buffer
 *
 * Unpacks CTA-861-G DRM infoframe DataBytes contained in the binary @buffer
 * into a structured @frame of the HDMI Dynamic Range and Mastering (DRM)
 * infoframe.
 *
 * Returns 0 on success or a negative error code on failure.
 */
int hdmi_drm_infoframe_unpack_only(struct hdmi_drm_infoframe *frame,
                                   const void *buffer, size_t size)
{
        const u8 *ptr = buffer;
        const u8 *temp;
        u8 x_lsb, x_msb;
        u8 y_lsb, y_msb;
        int ret;
        int i;

        if (size < HDMI_DRM_INFOFRAME_SIZE)
                return -EINVAL;

        ret = hdmi_drm_infoframe_init(frame);
        if (ret)
                return ret;

        frame->eotf = ptr[0] & 0x7;
        frame->metadata_type = ptr[1] & 0x7;

        temp = ptr + 2;
        for (i = 0; i < 3; i++) {
                x_lsb = *temp++;
                x_msb = *temp++;
                frame->display_primaries[i].x = (x_msb << 8) | x_lsb;
                y_lsb = *temp++;
                y_msb = *temp++;
                frame->display_primaries[i].y = (y_msb << 8) | y_lsb;
        }

        frame->white_point.x = (ptr[15] << 8) | ptr[14];
        frame->white_point.y = (ptr[17] << 8) | ptr[16];

        frame->max_display_mastering_luminance = (ptr[19] << 8) | ptr[18];
        frame->min_display_mastering_luminance = (ptr[21] << 8) | ptr[20];
        frame->max_cll = (ptr[23] << 8) | ptr[22];
        frame->max_fall = (ptr[25] << 8) | ptr[24];

        return 0;
}
EXPORT_SYMBOL(hdmi_drm_infoframe_unpack_only);
#endif /*LINUX_VERSION_IS_LESS(5,8,0)*/
