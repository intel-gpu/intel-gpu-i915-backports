# SPDX-License-Identifier: GPL-2.0-only
ifeq (y,$(filter y,$(CPTCFG_ONLY_DMABUF) $(CPTCFG_BUILD_DMABUF_I915_MEI)))
	ccflags-y += -I$(src)
ifeq ($(CONFIG_BACKPORT_INTEGRATE),)
	obj-m += dmabuf.o
else
	obj-y += dmabuf.o
endif

dmabuf-y +=  ../../compat/dmabuf-main.o
dmabuf-$(CPTCFG_KERNEL_5_1)  += ../../compat/backport-5.1.o
dmabuf-$(CPTCFG_KERNEL_5_10) += ../../compat/backport-5.10.o
dmabuf-y +=  ../../compat/backport-3.17.o ../../compat/fs_d_path.o

dmabuf-y += dma-buf.o dma-fence.o dma-fence-array.o dma-fence-chain.o \
         dma-resv.o seqno-fence.o

dmabuf-$(CPTCFG_DMABUF_HEAPS)	+= dma-heap.o
obj-$(CPTCFG_DMABUF_HEAPS)	+= heaps/
dmabuf-$(CPTCFG_SYNC_FILE)		+= sync_file.o
dmabuf-$(CPTCFG_SW_SYNC)		+= sw_sync.o sync_debug.o
dmabuf-$(CPTCFG_UDMABUF)		+= udmabuf.o
dmabuf-$(CPTCFG_DMABUF_SYSFS_STATS) += dma-buf-sysfs-stats.o

dmabuf_selftests-y := \
	selftest.o \
	st-dma-fence.o \
	st-dma-fence-chain.o

obj-$(CPTCFG_DMABUF_SELFTESTS)	+= dmabuf_selftests.o
endif
