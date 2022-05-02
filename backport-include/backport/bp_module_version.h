#include <linux/module.h>

#define BACKPORT_MOD_VER                        \
        "backported to " CPTCFG_BASE_KERNEL_NAME  \
        " from" " (" CPTCFG_DII_KERNEL_HEAD ")"         \
        " using backports " CPTCFG_BACKPORTS_RELEASE_TAG
