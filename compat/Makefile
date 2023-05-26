ccflags-y += -I$(src) -Wframe-larger-than=1280

ifeq (y,$(filter y,$(CPTCFG_ONLY_I915) $(CPTCFG_BOTH_DMABUF_I915)))
ifeq ($(CONFIG_BACKPORT_INTEGRATE),)
obj-m += i915-compat.o
else
obj-y += i915-compat.o
endif
endif

i915-compat-y += main.o

# Kernel backport compatibility code
i915-compat-$(CPTCFG_KERNEL_3_11) += backport-3.11.o
i915-compat-$(CPTCFG_KERNEL_3_12) += backport-3.12.o
i915-compat-$(CPTCFG_KERNEL_3_13) += backport-3.13.o memneq.o
i915-compat-$(CPTCFG_KERNEL_3_14) += backport-3.14.o
i915-compat-$(CPTCFG_KERNEL_3_15) += backport-3.15.o
i915-compat-$(CPTCFG_KERNEL_3_17) += backport-3.17.o
i915-compat-$(CPTCFG_KERNEL_5_7)  += backport-5.7.o
i915-compat-$(CPTCFG_KERNEL_5_8)  += backport-5.8.o
i915-compat-$(CPTCFG_KERNEL_5_4)  += plist.o ptrace.o
i915-compat-$(CPTCFG_KERNEL_5_5)  += nvmem-core.o
i915-compat-$(CPTCFG_KERNEL_5_10) += slub.o slab.o
i915-compat-$(CPTCFG_KERNEL_5_13) += backport-5.13.o
i915-compat-$(CPTCFG_KERNEL_5_15) += backport-5.15.o

i915-compat-$(CPTCFG_BPAUTO_BUILD_SYSTEM_DATA_VERIFICATION) += verification/verify.o
i915-compat-$(CPTCFG_BPAUTO_BUILD_SYSTEM_DATA_VERIFICATION) += verification/pkcs7.asn1.o
i915-compat-$(CPTCFG_BPAUTO_BUILD_SYSTEM_DATA_VERIFICATION) += verification/pkcs7_verify.o
i915-compat-$(CPTCFG_BPAUTO_BUILD_SYSTEM_DATA_VERIFICATION) += verification/pkcs7_parser.o
i915-compat-$(CPTCFG_BPAUTO_BUILD_SYSTEM_DATA_VERIFICATION) += verification/x509.asn1.o
i915-compat-$(CPTCFG_BPAUTO_BUILD_SYSTEM_DATA_VERIFICATION) += verification/x509_akid.asn1.o
i915-compat-$(CPTCFG_BPAUTO_BUILD_SYSTEM_DATA_VERIFICATION) += verification/x509_cert_parser.o
i915-compat-$(CPTCFG_BPAUTO_BUILD_SYSTEM_DATA_VERIFICATION) += verification/x509_public_key.o
i915-compat-$(CPTCFG_BPAUTO_BUILD_SYSTEM_DATA_VERIFICATION) += verification/pkcs7_trust.o
i915-compat-$(CPTCFG_BPAUTO_BUILD_SYSTEM_DATA_VERIFICATION) += verification/key.o
i915-compat-$(CPTCFG_BPAUTO_BUILD_SYSTEM_DATA_VERIFICATION) += verification/public_key.o
i915-compat-$(CPTCFG_BPAUTO_BUILD_SYSTEM_DATA_VERIFICATION) += verification/rsa.o
i915-compat-$(CPTCFG_BPAUTO_BUILD_SYSTEM_DATA_VERIFICATION) += verification/bignum.o
i915-compat-$(CPTCFG_BPAUTO_BUILD_SYSTEM_DATA_VERIFICATION) += verification/md.o
i915-compat-$(CPTCFG_BPAUTO_BUILD_SYSTEM_DATA_VERIFICATION) += verification/md_wrap.o
i915-compat-$(CPTCFG_BPAUTO_BUILD_SYSTEM_DATA_VERIFICATION) += verification/sha256.o
i915-compat-$(CPTCFG_BPAUTO_BUILD_SYSTEM_DATA_VERIFICATION) += verification/oid.o
i915-compat-$(CPTCFG_BPAUTO_BUILD_SYSTEM_DATA_VERIFICATION) += verification/asn1parse.o
i915-compat-$(CPTCFG_BPAUTO_BUILD_SYSTEM_DATA_VERIFICATION) += verification/rsapubkey.asn1.o

$(obj)/lib-oid_registry.o: $(obj)/oid_registry_data.c

$(obj)/oid_registry_data.c: $(src)/../include/linux/backport-oid_registry.h \
			    $(src)/build_OID_registry
	$(call cmd,build_OID_registry)

quiet_cmd_build_OID_registry = GEN     $@
	cmd_build_OID_registry = perl $(src)/build_OID_registry $< $@
i915-compat-$(CPTCFG_BPAUTO_ASN1_DECODER) += lib-asn1_decoder.o
i915-compat-$(CPTCFG_BPAUTO_REFCOUNT) += lib-refcount.o
i915-compat-$(CPTCFG_BPAUTO_RHASHTABLE) += lib-rhashtable.o
i915-compat-$(CPTCFG_BPAUTO_DMA_FENCE_TRACE) += 
i915-compat-$(CPTCFG_BPAUTO_PUBLIC_KEY) += 
i915-compat-$(CPTCFG_BPAUTO_BUCKET_LOCKS) += lib-bucket_locks.o
i915-compat-$(CPTCFG_BPAUTO_PKCS7) += 
i915-compat-$(CPTCFG_BPAUTO_BUILD_HDMI) += drivers-video-hdmi.o
