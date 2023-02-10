#ifndef _BACKPORT_LINUX_HDMI_H_
#define _BACKPORT_LINUX_HDMI_H_
#include <linux/version.h>
#include_next <linux/hdmi.h>

#ifdef BPM_HDMI_DRM_INFOFRAME_UNPACK_NOT_PRESENT
int hdmi_drm_infoframe_unpack_only(struct hdmi_drm_infoframe *frame,
                                   const void *buffer, size_t size);
#endif

#define HDMI_PACKET_TYPE_EMP 0x7F

/* HDMI2.1 Extended Metadata Packet Sec: 8.8 */
enum hdmi_emp_type {
        HDMI_EMP_TYPE_VSEMDS,
        HDMI_EMP_TYPE_CVTEM,
        HDMI_EMP_TYPE_HDR_DMEI,
        HDMI_EMP_TYPE_VTEM,
};

enum hdmi_emp_ds_type {
        HDMI_EMP_DS_TYPE_PSTATIC,
        HDMI_EMP_DS_TYPE_DYNAMIC,
        HDMI_EMP_DS_TYPE_UNIQUE,
        HDMI_EMP_DS_TYPE_RESERVED,
};

struct hdmi_emp_header {
        u8 hb0;
        u8 hb1;
        u8 hb2;
};

struct hdmi_emp_first_dsf {
        bool pb0_new;
        bool pb0_end;
        bool pb0_afr;
        bool pb0_vfr;
        bool pb0_sync;
        enum hdmi_emp_ds_type ds_type;
        int org_id;
        int data_set_tag;
        int data_set_length;
};

struct hdmi_extended_metadata_packet {
        bool enabled;
        enum hdmi_emp_type type;
        struct hdmi_emp_header header;
        struct hdmi_emp_first_dsf first_data_set;
};

#ifdef BPM_VRR_SUPPORT_NOT_PRESENT
struct hdmi_vtem_payload {
        bool vrr_en;
        bool m_const;
        bool qms_en;
        bool rb;
        u8 fva_factor;
        u8 base_vfront;
        u8 next_tfr;
        u16 base_refresh_rate;
};

struct hdmi_video_timing_emp_config {
        struct hdmi_extended_metadata_packet vtemp;
        struct hdmi_vtem_payload payload;
};
#endif

#endif /*_BACKPORT_LINUX_HDMI_H_*/
