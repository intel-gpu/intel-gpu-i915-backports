ccflags-y += -I$(src) -Wframe-larger-than=1280
ifeq ($(CONFIG_BACKPORT_INTEGRATE),)
obj-m += compat.o
else
obj-y += compat.o
endif
compat-y += main.o

# Kernel backport compatibility code
compat-$(CPTCFG_KERNEL_3_0) += compat-3.0.o
compat-$(CPTCFG_KERNEL_3_1) += compat-3.1.o
compat-$(CPTCFG_KERNEL_3_2) += backport-3.2.o
compat-$(CPTCFG_KERNEL_3_3) += compat-3.3.o
compat-$(CPTCFG_KERNEL_3_4) += compat-3.4.o
compat-$(CPTCFG_KERNEL_3_5) += compat-3.5.o user_namespace.o
compat-$(CPTCFG_KERNEL_3_6) += compat-3.6.o
compat-$(CPTCFG_KERNEL_3_7) += compat-3.7.o
compat-$(CPTCFG_KERNEL_3_8) += compat-3.8.o
compat-$(CPTCFG_KERNEL_3_9) += compat-3.9.o
compat-$(CPTCFG_KERNEL_3_10) += backport-3.10.o
compat-$(CPTCFG_KERNEL_3_11) += backport-3.11.o
compat-$(CPTCFG_KERNEL_3_12) += backport-3.12.o
compat-$(CPTCFG_KERNEL_3_13) += backport-3.13.o memneq.o
compat-$(CPTCFG_KERNEL_3_14) += backport-3.14.o
compat-$(CPTCFG_KERNEL_3_15) += backport-3.15.o
compat-$(CPTCFG_KERNEL_3_17) += backport-3.17.o
compat-$(CPTCFG_KERNEL_3_18) += backport-3.18.o
compat-$(CPTCFG_KERNEL_3_19) += backport-3.19.o
compat-$(CPTCFG_KERNEL_4_0) += backport-4.0.o
compat-$(CPTCFG_KERNEL_4_1) += backport-4.1.o
compat-$(CPTCFG_KERNEL_4_2) += backport-4.2.o
compat-$(CPTCFG_KERNEL_4_3) += backport-4.3.o
compat-$(CPTCFG_KERNEL_4_4) += backport-4.4.o
compat-$(CPTCFG_KERNEL_4_5) += backport-4.5.o
compat-$(CPTCFG_KERNEL_4_6) += backport-4.6.o
compat-$(CPTCFG_KERNEL_4_7) += backport-4.7.o
compat-$(CPTCFG_KERNEL_4_8) += backport-4.8.o
compat-$(CPTCFG_KERNEL_4_10) += backport-4.10.o
compat-$(CPTCFG_KERNEL_4_18) += backport-4.18.o
compat-$(CPTCFG_KERNEL_5_2) += backport-5.2.o backport-genetlink.o
compat-$(CPTCFG_KERNEL_5_5) += backport-5.5.o
compat-y += backport-5.10.o
compat-y += slub.o slab.o ptrace.o
