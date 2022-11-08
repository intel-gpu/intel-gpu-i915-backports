#ifndef __BACKPORT_BACKLIGHT_H
#define __BACKPORT_BACKLIGHT_H
#include_next <linux/backlight.h>

#ifdef BACKLIGHT_DEV_GET_BY_NAME_NOT_PRESENT

struct backlight_device *backlight_device_get_by_name(const char *name);
#endif

#endif /* __BACKPORT_BACKLIGHT_H */
