TARGET_KERNEL_VERSION=`uname -r`
if [ ! -d /lib/modules/${TARGET_KERNEL_VERSION}/extra/ukmd/ ]; then
	mkdir -p /lib/modules/${TARGET_KERNEL_VERSION}/extra/ukmd/
fi
#cd orig
#patch -p1< ../dualdrm/for-centos-7.5.patch
#cd -
#cd orig/drivers/gpu/drm
#make -C /usr/src/kernels/${TARGET_KERNEL_VERSION}/  ARCH=x86 modules M=$PWD
#cd -

#cp -f ./orig/drivers/gpu/drm/drm.ko /lib/modules/${TARGET_KERNEL_VERSION}/extra/ukmd/drm.ko
#cp -f ./orig/drivers/gpu/drm/Module.symvers ./Module_centos7.4.symvers

make usedefconfig
make -j8
cp -f ./compat/drm_compat.ko /lib/modules/${TARGET_KERNEL_VERSION}/extra/ukmd/
cp -f ./drivers/gpu/drm/drm.ko /lib/modules/${TARGET_KERNEL_VERSION}/extra/ukmd/
cp -f ./drivers/gpu/drm/drm_kms_helper.ko /lib/modules/${TARGET_KERNEL_VERSION}/extra/ukmd/
cp -f ./drivers/gpu/drm/i915/i915.ko /lib/modules/${TARGET_KERNEL_VERSION}/extra/ukmd/

depmod -a ${TARGET_KERNEL_VERSION}

echo -e $ECHO_PREFIX_INFO "Calling mkinitrd upon ${TARGET_KERNEL_VERSION} kernel..."
mkinitrd --force /boot/initramfs-"${TARGET_KERNEL_VERSION}".img ${TARGET_KERNEL_VERSION}

