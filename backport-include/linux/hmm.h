#ifndef __BACKPORT_LINUX_HMM_H
#define __BACKPORT_LINUX_HMM_H
#include_next <linux/hmm.h>
#include <linux/version.h>

#ifdef BPM_MIGRATE_AND_MEMREMAP_NOT_PRESENT

#include <linux/migrate.h>
#include <linux/memremap.h>

#endif
#endif /* __BACKPORT_LINUX_HWMON_H */
