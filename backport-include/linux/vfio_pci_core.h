/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 Red Hat, Inc.  All rights reserved.
 * Author: Alex Williamson <alex.williamson@redhat.com>
 * Derived from original vfio:
 * Copyright 2010 Cisco Systems, Inc.  All rights reserved.
 * Author: Tom Lyon, pugs@cisco.com
 *
 */

#ifndef _BACKPORT_VFIO_PCI_CORE_H
#define _BACKPORT_VFIO_PCI_CORE_H

#ifndef BPM_VFIO_PCI_CORE_HEADER_NOT_PRESENT
#include_next <linux/vfio_pci_core.h>
#endif

#ifdef BPM_VFIO_PCI_CORE_UN_INIT_DEVICE_API_NOT_PRESENT

static inline int vfio_pci_core_init_device(struct vfio_pci_core_device *vdev,
		struct pci_dev *pdev,
		const struct vfio_device_ops *vfio_pci_ops)
{
	return vfio_pci_core_init_dev(&vdev->vdev);
}

static inline void vfio_pci_core_uninit_device(struct vfio_pci_core_device *vdev)
{
	return vfio_pci_core_release_dev(&vdev->vdev);
}

#endif /* BPM_VFIO_PCI_CORE_UN_INIT_DEVICE_API_NOT_PRESENT  */

#endif /* _BACKPORT_VFIO_PCI_CORE_H */
