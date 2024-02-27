/*
 * Copyright (c) 2014  Hauke Mehrtens <hauke@hauke-m.de>
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/module.h>
#include <linux/acpi.h>

/**
 * acpi_evaluate_dsm - evaluate device's _DSM method
 * @handle: ACPI device handle
 * @uuid: UUID of requested functions, should be 16 bytes
 * @rev: revision number of requested function
 * @func: requested function number
 * @argv4: the function specific parameter
 *
 * Evaluate device's _DSM method with specified UUID, revision id and
 * function number. Caller needs to free the returned object.
 *
 * Though ACPI defines the fourth parameter for _DSM should be a package,
 * some old BIOSes do expect a buffer or an integer etc.
 */
union acpi_object *
acpi_evaluate_dsm(acpi_handle handle, const u8 *uuid, int rev, int func,
                  union acpi_object *argv4)
{
        acpi_status ret;
        struct acpi_buffer buf = {ACPI_ALLOCATE_BUFFER, NULL};
        union acpi_object params[4];
        struct acpi_object_list input = {
                .count = 4,
                .pointer = params,
        };

        params[0].type = ACPI_TYPE_BUFFER;
        params[0].buffer.length = 16;
        params[0].buffer.pointer = (char *)uuid;
        params[1].type = ACPI_TYPE_INTEGER;
        params[1].integer.value = rev;
        params[2].type = ACPI_TYPE_INTEGER;
        params[2].integer.value = func;
	if (argv4) {
                params[3] = *argv4;
        } else {
                params[3].type = ACPI_TYPE_PACKAGE;
                params[3].package.count = 0;
                params[3].package.elements = NULL;
        }

        ret = acpi_evaluate_object(handle, "_DSM", &input, &buf);
        if (ACPI_SUCCESS(ret))
                return (union acpi_object *)buf.pointer;

        if (ret != AE_NOT_FOUND)
                acpi_handle_warn(handle,
                                "failed to evaluate _DSM (0x%x)\n", ret);

        return NULL;
}
EXPORT_SYMBOL(acpi_evaluate_dsm);

/**
 * acpi_check_dsm - check if _DSM method supports requested functions.
 * @handle: ACPI device handle
 * @uuid: UUID of requested functions, should be 16 bytes at least
 * @rev: revision number of requested functions
 * @funcs: bitmap of requested functions
 *
 * Evaluate device's _DSM method to check whether it supports requested
 * functions. Currently only support 64 functions at maximum, should be
 * enough for now.
 */
bool acpi_check_dsm(acpi_handle handle, const u8 *uuid, int rev, u64 funcs)
{
        int i;
        u64 mask = 0;
        union acpi_object *obj;

        if (funcs == 0)
                return false;

        obj = acpi_evaluate_dsm(handle, uuid, rev, 0, NULL);
        if (!obj)
                return false;

        /* For compatibility, old BIOSes may return an integer */
        if (obj->type == ACPI_TYPE_INTEGER)
                mask = obj->integer.value;
        else if (obj->type == ACPI_TYPE_BUFFER)
                for (i = 0; i < obj->buffer.length && i < 8; i++)
                        mask |= (((u8)obj->buffer.pointer[i]) << (i * 8));
        ACPI_FREE(obj);

        /*
         * Bit 0 indicates whether there's support for any functions other than
         * function 0 for the specified UUID and revision.
         */
	if ((mask & 0x1) && (mask & funcs) == funcs)
                return true;

        return false;
}
EXPORT_SYMBOL(acpi_check_dsm);

#ifdef CONFIG_PCI_MSI
/**
 * pci_enable_msi_range - configure device's MSI capability structure
 * @dev: device to configure
 * @minvec: minimal number of interrupts to configure
 * @maxvec: maximum number of interrupts to configure
 *
 * This function tries to allocate a maximum possible number of interrupts in a
 * range between @minvec and @maxvec. It returns a negative errno if an error
 * occurs. If it succeeds, it returns the actual number of interrupts allocated
 * and updates the @dev's irq member to the lowest new interrupt number;
 * the other interrupt numbers allocated to this device are consecutive.
 **/
int pci_enable_msi_range(struct pci_dev *dev, int minvec, int maxvec)
{
	int nvec = maxvec;
	int rc;

	if (maxvec < minvec)
		return -ERANGE;

	do {
		rc = pci_enable_msi_block(dev, nvec);
		if (rc < 0) {
			return rc;
		} else if (rc > 0) {
			if (rc < minvec)
				return -ENOSPC;
			nvec = rc;
		}
	} while (rc);

	return nvec;
}
EXPORT_SYMBOL(pci_enable_msi_range);
#endif

#ifdef CONFIG_PCI_MSI
/**
 * pci_enable_msix_range - configure device's MSI-X capability structure
 * @dev: pointer to the pci_dev data structure of MSI-X device function
 * @entries: pointer to an array of MSI-X entries
 * @minvec: minimum number of MSI-X irqs requested
 * @maxvec: maximum number of MSI-X irqs requested
 *
 * Setup the MSI-X capability structure of device function with a maximum
 * possible number of interrupts in the range between @minvec and @maxvec
 * upon its software driver call to request for MSI-X mode enabled on its
 * hardware device function. It returns a negative errno if an error occurs.
 * If it succeeds, it returns the actual number of interrupts allocated and
 * indicates the successful configuration of MSI-X capability structure
 * with new allocated MSI-X interrupts.
 **/
int pci_enable_msix_range(struct pci_dev *dev, struct msix_entry *entries,
			       int minvec, int maxvec)
{
	int nvec = maxvec;
	int rc;

	if (maxvec < minvec)
		return -ERANGE;

	do {
		rc = pci_enable_msix(dev, entries, nvec);
		if (rc < 0) {
			return rc;
		} else if (rc > 0) {
			if (rc < minvec)
				return -ENOSPC;
			nvec = rc;
		}
	} while (rc);

	return nvec;
}
EXPORT_SYMBOL(pci_enable_msix_range);
#endif
