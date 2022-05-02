# SPDX-License-Identifier: GPL-2.0-only
ifeq (y,$(filter y,$(CPTCFG_ONLY_DMABUF) $(CPTCFG_BOTH_DMABUF_I915)))
ccflags-y += -I$(src)
ifeq ($(CONFIG_BACKPORT_INTEGRATE),)
obj-m += dmabuf.o
else
obj-y += dmabuf.o
endif

dmabuf-y +=  ../../compat/dmabuf-main.o
dmabuf-$(CPTCFG_KERNEL_5_4)  += ../../compat/backport-5.3.o
dmabuf-$(CPTCFG_KERNEL_5_10)  += ../../compat/backport-5.10.o

dmabuf-y += dma-buf.o dma-fence.o dma-fence-array.o dma-fence-chain.o \
	 dma-resv.o seqno-fence.o
dmabuf-$(CPTCFG_DMABUF_HEAPS)	+= dma-heap.o
obj-$(CPTCFG_DMABUF_HEAPS)	+= heaps/
dmabuf-$(CPTCFG_SYNC_FILE)	+= sync_file.o
dmabuf-$(CPTCFG_SW_SYNC)	+= sw_sync.o sync_debug.o

dmabuf_selftests-y := \
	selftest.o \
	st-dma-fence.o \
	st-dma-fence-chain.o

obj-$(CPTCFG_DMABUF_SELFTESTS)	+= dmabuf_selftests.o

# Enable shims for the RHEL DRM backport
ccflags-y += -DRH_DRM_BACKPORT
endif
