#ifndef __BACKPORT_PWM_H
#define __BACKPORT_PWM_H
#include_next <linux/pwm.h>

#ifdef BPM_MODULE_IMPORT_TO_STRING_LITERAL_PRESENT
MODULE_IMPORT_NS(PWM);
#endif

#endif /* __BACKPORT_PWM_H */
