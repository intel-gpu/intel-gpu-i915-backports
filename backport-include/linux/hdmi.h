#ifndef _BACKPORT_LINUX_HDMI_H_
#define _BACKPORT_LINUX_HDMI_H_
#include <linux/version.h>
#include_next <linux/hdmi.h>

#if LINUX_VERSION_IS_LESS(5,8,0)
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

#endif /*_BACKPORT_LINUX_HDMI_H_*/

