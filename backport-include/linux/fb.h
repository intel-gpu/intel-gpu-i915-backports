#ifndef _BACKPORT_LINUX_FB_H
#define _BACKPORT_LINUX_FB_H
#include_next<linux/fb.h>
#ifdef BPM_FB_ACTIVATE_KD_TEXT_NOT_PRESENT
#define FB_ACTIVATE_KD_TEXT   512       /* for KDSET vt ioctl */
#endif
#endif

