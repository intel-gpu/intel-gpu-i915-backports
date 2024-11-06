/* SPDX-License-Identifier: MIT */

#include <linux/pci.h>
#include <linux/types.h>

int i915_sriov_pause_vf(struct pci_dev *pdev, unsigned int vfid);
int i915_sriov_resume_vf(struct pci_dev *pdev, unsigned int vfid);

int i915_sriov_wait_vf_flr_done(struct pci_dev *pdev, unsigned int vfid);

ssize_t
i915_sriov_ggtt_size(struct pci_dev *pdev, unsigned int vfid, unsigned int tile);
ssize_t i915_sriov_ggtt_save(struct pci_dev *pdev, unsigned int vfid, unsigned int tile,
			     void *buf, size_t size);
int
i915_sriov_ggtt_load(struct pci_dev *pdev, unsigned int vfid, unsigned int tile,
		     const void *buf, size_t size);

ssize_t
i915_sriov_fw_state_size(struct pci_dev *pdev, unsigned int vfid,
			 unsigned int tile);
ssize_t
i915_sriov_fw_state_save(struct pci_dev *pdev, unsigned int vfid, unsigned int tile,
			 void *buf, size_t size);
int
i915_sriov_fw_state_load(struct pci_dev *pdev, unsigned int vfid, unsigned int tile,
			 const void *buf, size_t size);

void *i915_sriov_smem_alloc(struct pci_dev *pdev, size_t size);
void i915_sriov_smem_free(struct pci_dev *pdev, const void *vaddr);

ssize_t
i915_sriov_lmem_size(struct pci_dev *pdev, unsigned int vfid, unsigned int tile);
void *i915_sriov_lmem_map(struct pci_dev *pdev, unsigned int vfid, unsigned int tile);
void
i915_sriov_lmem_unmap(struct pci_dev *pdev, unsigned int vfid, unsigned int tile);
ssize_t
i915_sriov_lmem_save(struct pci_dev *pdev, unsigned int vfid, unsigned int tile, void *buf,
		     u64 offset, size_t size);
int
i915_sriov_lmem_load(struct pci_dev *pdev, unsigned int vfid, unsigned int tile, const void *buf,
		     u64 offset, size_t size);

ssize_t
i915_sriov_ccs_size(struct pci_dev *pdev, unsigned int vfid, unsigned int tile);
ssize_t
i915_sriov_ccs_save(struct pci_dev *pdev, unsigned int vfid, unsigned int tile, void *buf,
		    u64 offset, size_t size);
int
i915_sriov_ccs_load(struct pci_dev *pdev, unsigned int vfid, unsigned int tile, const void *buf,
		    u64 offset, size_t size);
