/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Sharan Santhanam <sharan.santhanam@neclab.eu>
 *
 * Copyright (c) 2018, NEC Europe Ltd., NEC Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <uk/config.h>
#include <uk/arch/types.h>
#include <errno.h>
#include <uk/alloc.h>
#include <uk/print.h>
#include <uk/lcpu.h>
#include <uk/intctlr.h>
#include <uk/arch/util.h>
#include <uk/bus/pci.h>
#include <virtio/virtio_config.h>
#include <virtio/virtio_bus.h>
#include <virtio/virtqueue.h>
#include <virtio/virtio_pci.h>

#define VENDOR_QUMRANET_VIRTIO           (0x1AF4)
#define VIRTIO_PCI_MODERN_DEVICEID_START (0x1040)
#define VIRTIO_PCI_MODERN_DEVICEID_END   (0x107f)

/*
 * Cloud Hypervisor compatibility: modern virtio-pci devices expose their
 * transport regions through vendor-specific PCI capabilities and use MSI-X.
 */
#define PCI_STATUS_CAP_LIST              0x10
#define PCI_CAP_ID_VNDR                  0x09
#define PCI_CAP_ID_MSIX                  0x11

#define PCI_MSIX_FLAGS                   2
#define PCI_MSIX_TABLE                   4
#define PCI_MSIX_FLAGS_ENABLE            0x8000
#define PCI_MSIX_FLAGS_MASKALL           0x4000
#define PCI_MSIX_TABLE_BIR               0x7
#define PCI_MSIX_TABLE_OFFSET            (~0x7U)

#define PCI_MSIX_ENTRY_ADDR_LO           0
#define PCI_MSIX_ENTRY_ADDR_HI           4
#define PCI_MSIX_ENTRY_DATA              8
#define PCI_MSIX_ENTRY_VECTOR_CTRL       12
#define PCI_MSIX_ENTRY_MASKED            1

#define X86_MSI_ADDRESS                  0xfee00000U
#define X86_MSI_VECTOR_BASE              32

#define VIRTIO_PCI_CAP_COMMON_CFG        1
#define VIRTIO_PCI_CAP_NOTIFY_CFG        2
#define VIRTIO_PCI_CAP_ISR_CFG           3
#define VIRTIO_PCI_CAP_DEVICE_CFG        4

#define VIRTIO_PCI_CAP_CFG_TYPE          3
#define VIRTIO_PCI_CAP_BAR               4
#define VIRTIO_PCI_CAP_OFFSET            8
#define VIRTIO_PCI_CAP_LENGTH            12
#define VIRTIO_PCI_NOTIFY_MULTIPLIER     16

#define VIRTIO_PCI_COMMON_DFSELECT       0
#define VIRTIO_PCI_COMMON_DF             4
#define VIRTIO_PCI_COMMON_GFSELECT       8
#define VIRTIO_PCI_COMMON_GF             12
#define VIRTIO_PCI_COMMON_MSIX           16
#define VIRTIO_PCI_COMMON_NUMQ           18
#define VIRTIO_PCI_COMMON_STATUS         20
#define VIRTIO_PCI_COMMON_QSELECT        22
#define VIRTIO_PCI_COMMON_QSIZE          24
#define VIRTIO_PCI_COMMON_QMSIX          26
#define VIRTIO_PCI_COMMON_QENABLE        28
#define VIRTIO_PCI_COMMON_QNOFF          30
#define VIRTIO_PCI_COMMON_QDESC          32
#define VIRTIO_PCI_COMMON_QDRIVER        40
#define VIRTIO_PCI_COMMON_QDEVICE        48

#define VIRTIO_PCI_MSIX_NO_VECTOR        0xffff

/* x86 KVM boot page tables direct-map the first 512 GiB at -512 GiB. */
#define VIRTIO_PCI_X86_DIRECTMAP_START   0xffffff8000000000ULL

static struct uk_alloc *a;

/**
 * The structure declares a pci device.
 */
struct virtio_pci_dev {
	/* Virtio Device */
	struct virtio_dev vdev;
	/* Pci base address */
	__u64 pci_base_addr;
	/* ISR Address Range */
	__u64 pci_isr_addr;
	/* Pci device information */
	struct pci_device *pdev;
	/* Modern virtio PCI capability mappings */
	void *common_cfg;
	void *notify_cfg;
	void *device_cfg;
	__u32 notify_off_multiplier;
	__bool modern;
	void *msix_table;
	__u8 msix_capability;
	unsigned int msix_irq;
	__bool msix_enabled;
};

/**
 * Fetch the virtio pci information from the virtio device.
 * @param vdev
 *	Reference to the virtio device.
 */
#define to_virtiopcidev(vdev) \
		__containerof(vdev, struct virtio_pci_dev, vdev)

/**
 * Static function declaration.
 */
static void vpci_legacy_pci_dev_reset(struct virtio_dev *vdev);
static int vpci_legacy_pci_config_set(struct virtio_dev *vdev, __u16 offset,
				      const void *buf, __u32 len);
static int vpci_legacy_pci_config_get(struct virtio_dev *vdev, __u16 offset,
				      void *buf, __u32 len, __u8 type_len);
static __u64 vpci_legacy_pci_features_get(struct virtio_dev *vdev);
static void vpci_legacy_pci_features_set(struct virtio_dev *vdev);
static int vpci_legacy_pci_vq_find(struct virtio_dev *vdev, __u16 num_vq,
				   __u16 *qdesc_size);
static void vpci_legacy_pci_status_set(struct virtio_dev *vdev, __u8 status);
static __u8 vpci_legacy_pci_status_get(struct virtio_dev *vdev);
static struct virtqueue *vpci_legacy_vq_setup(struct virtio_dev *vdev,
					      __u16 queue_id,
					      __u16 num_desc,
					      virtqueue_callback_t callback,
					      struct uk_alloc *a);
static void vpci_legacy_vq_release(struct virtio_dev *vdev,
		struct virtqueue *vq, struct uk_alloc *a);
static int virtio_pci_handle(void *arg);
static int vpci_legacy_notify(struct virtio_dev *vdev, __u16 queue_id);
static int virtio_pci_legacy_add_dev(struct pci_device *pci_dev,
				     struct virtio_pci_dev *vpci_dev);
static int virtio_pci_modern_add_dev(struct pci_device *pci_dev,
				     struct virtio_pci_dev *vpci_dev);

/**
 * Configuration operations legacy PCI device.
 */
static struct virtio_config_ops vpci_legacy_ops = {
	.device_reset = vpci_legacy_pci_dev_reset,
	.config_get   = vpci_legacy_pci_config_get,
	.config_set   = vpci_legacy_pci_config_set,
	.features_get = vpci_legacy_pci_features_get,
	.features_set = vpci_legacy_pci_features_set,
	.status_get   = vpci_legacy_pci_status_get,
	.status_set   = vpci_legacy_pci_status_set,
	.vqs_find     = vpci_legacy_pci_vq_find,
	.vq_setup     = vpci_legacy_vq_setup,
	.vq_release   = vpci_legacy_vq_release,
};

/*
 * Cloud Hypervisor compatibility: transport operations for modern
 * virtio-pci. The original legacy operation table above remains unchanged.
 */
static void vpci_modern_pci_dev_reset(struct virtio_dev *vdev);
static int vpci_modern_pci_config_set(struct virtio_dev *vdev, __u16 offset,
				      const void *buf, __u32 len);
static int vpci_modern_pci_config_get(struct virtio_dev *vdev, __u16 offset,
				      void *buf, __u32 len, __u8 type_len);
static __u64 vpci_modern_pci_features_get(struct virtio_dev *vdev);
static void vpci_modern_pci_features_set(struct virtio_dev *vdev);
static int vpci_modern_pci_vq_find(struct virtio_dev *vdev, __u16 num_vq,
				   __u16 *qdesc_size);
static void vpci_modern_pci_status_set(struct virtio_dev *vdev, __u8 status);
static __u8 vpci_modern_pci_status_get(struct virtio_dev *vdev);
static struct virtqueue *vpci_modern_vq_setup(struct virtio_dev *vdev,
					      __u16 queue_id,
					      __u16 num_desc,
					      virtqueue_callback_t callback,
					      struct uk_alloc *a);
static void vpci_modern_vq_release(struct virtio_dev *vdev,
				   struct virtqueue *vq, struct uk_alloc *a);
static int vpci_modern_notify(struct virtio_dev *vdev, __u16 queue_id);

static struct virtio_config_ops vpci_modern_ops = {
	.device_reset = vpci_modern_pci_dev_reset,
	.config_get   = vpci_modern_pci_config_get,
	.config_set   = vpci_modern_pci_config_set,
	.features_get = vpci_modern_pci_features_get,
	.features_set = vpci_modern_pci_features_set,
	.status_get   = vpci_modern_pci_status_get,
	.status_set   = vpci_modern_pci_status_set,
	.vqs_find     = vpci_modern_pci_vq_find,
	.vq_setup     = vpci_modern_vq_setup,
	.vq_release   = vpci_modern_vq_release,
};

#if CONFIG_ARCH_X86_64
static __u32 vpci_config_addr(const struct pci_device *pdev, __u8 offset)
{
	return PCI_ENABLE_BIT |
		((__u32)pdev->addr.bus << PCI_BUS_SHIFT) |
		((__u32)pdev->addr.devid << PCI_DEVICE_SHIFT) |
		((__u32)pdev->addr.function << PCI_FUNCTION_SHIFT) |
		(offset & ~3U);
}

static __u32 vpci_config_read32(const struct pci_device *pdev, __u8 offset)
{
	uk_arch_x86_64_outl(PCI_CONFIG_ADDR, vpci_config_addr(pdev, offset));
	return uk_arch_x86_64_inl(PCI_CONFIG_DATA);
}

static __u16 vpci_config_read16(const struct pci_device *pdev, __u8 offset)
{
	return (__u16)(vpci_config_read32(pdev, offset) >>
			((offset & 2U) * 8U));
}

static __u8 vpci_config_read8(const struct pci_device *pdev, __u8 offset)
{
	return (__u8)(vpci_config_read32(pdev, offset) >>
			((offset & 3U) * 8U));
}

static void vpci_config_write16(const struct pci_device *pdev, __u8 offset,
				__u16 value)
{
	__u32 data;
	__u32 shift = (offset & 2U) * 8U;

	data = vpci_config_read32(pdev, offset);
	data = (data & ~(0xffffU << shift)) | ((__u32)value << shift);
	uk_arch_x86_64_outl(PCI_CONFIG_ADDR, vpci_config_addr(pdev, offset));
	uk_arch_x86_64_outl(PCI_CONFIG_DATA, data);
}

static __u64 vpci_bar_base(const struct pci_device *pdev, __u8 bar)
{
	__u8 offset;
	__u32 low;
	__u64 base;

	if (bar > 5)
		return 0;

	offset = PCI_BASE_ADDRESS_0 + bar * sizeof(__u32);
	low = vpci_config_read32(pdev, offset);
	if (!low || (low & 1U))
		return 0;

	base = low & ~0xfULL;
	if ((low & 0x6U) == 0x4U) {
		if (bar == 5)
			return 0;
		base |= (__u64)vpci_config_read32(pdev,
				offset + sizeof(__u32)) << 32;
	}

	return base;
}
#endif /* CONFIG_ARCH_X86_64 */

static void vpci_modern_write64(void *base, __u8 offset, __u64 value)
{
	virtio_mmio_cwrite32(base, offset, (__u32)value);
	virtio_mmio_cwrite32(base, offset + sizeof(__u32), (__u32)(value >> 32));
}

static int vpci_modern_enable_msix(struct virtio_pci_dev *vpdev)
{
	__u16 flags;
	int rc;

	if (!vpdev->msix_table || !vpdev->msix_capability)
		return -ENODEV;

	rc = uk_intctlr_irq_alloc(&vpdev->msix_irq, 1);
	if (rc)
		return rc;
	rc = uk_intctlr_irq_register(vpdev->msix_irq, virtio_pci_handle, vpdev);
	if (rc) {
		uk_intctlr_irq_free(&vpdev->msix_irq, 1);
		return rc;
	}

	/*
	 * Cloud Hypervisor compatibility: route modern virtio interrupts through
	 * MSI-X vector 0 to the bootstrap processor's local APIC.
	 */
	virtio_mmio_cwrite32(vpdev->msix_table,
			     PCI_MSIX_ENTRY_VECTOR_CTRL,
			     PCI_MSIX_ENTRY_MASKED);
	virtio_mmio_cwrite32(vpdev->msix_table, PCI_MSIX_ENTRY_ADDR_LO,
			     X86_MSI_ADDRESS);
	virtio_mmio_cwrite32(vpdev->msix_table, PCI_MSIX_ENTRY_ADDR_HI, 0);
	virtio_mmio_cwrite32(vpdev->msix_table, PCI_MSIX_ENTRY_DATA,
			     vpdev->msix_irq + X86_MSI_VECTOR_BASE);
	uk_arch_wmb();
	virtio_mmio_cwrite32(vpdev->msix_table,
			     PCI_MSIX_ENTRY_VECTOR_CTRL, 0);

	flags = vpci_config_read16(vpdev->pdev,
				   vpdev->msix_capability + PCI_MSIX_FLAGS);
	flags |= PCI_MSIX_FLAGS_ENABLE;
	flags &= ~PCI_MSIX_FLAGS_MASKALL;
	vpci_config_write16(vpdev->pdev,
			    vpdev->msix_capability + PCI_MSIX_FLAGS, flags);
	vpdev->msix_enabled = 1;
	uk_pr_info("Modern virtio-pci MSI-X vector 0 uses IRQ %u (IDT %u)\n",
		   vpdev->msix_irq, vpdev->msix_irq + X86_MSI_VECTOR_BASE);
	return 0;
}

static int vpci_modern_notify(struct virtio_dev *vdev, __u16 queue_id)
{
	struct virtio_pci_dev *vpdev = to_virtiopcidev(vdev);
	__u16 notify_offset;
	void *queue_notify;

	/* Modern devices notify each queue through its capability-derived doorbell. */
	virtio_mmio_cwrite16(vpdev->common_cfg, VIRTIO_PCI_COMMON_QSELECT,
			      queue_id);
	notify_offset = virtio_mmio_cread16(vpdev->common_cfg,
					   VIRTIO_PCI_COMMON_QNOFF);
	queue_notify = (void *)((__uptr)vpdev->notify_cfg +
			       (__uptr)notify_offset *
			       vpdev->notify_off_multiplier);
	virtio_mmio_cwrite16(queue_notify, 0, queue_id);
	return 0;
}

static int vpci_legacy_notify(struct virtio_dev *vdev, __u16 queue_id)
{
	struct virtio_pci_dev *vpdev;

	UK_ASSERT(vdev);
	vpdev = to_virtiopcidev(vdev);
	virtio_cwrite16((void *)(unsigned long) vpdev->pci_base_addr,
			VIRTIO_PCI_QUEUE_NOTIFY, queue_id);

	return 0;
}

static int virtio_pci_handle(void *arg)
{
	struct virtio_pci_dev *d = (struct virtio_pci_dev *) arg;
	__u8 isr_status;
	struct virtqueue *vq;
	int rc = 0;

	UK_ASSERT(arg);

	/* Reading the isr status is used to acknowledge the interrupt */
	if (d->msix_enabled) {
		UK_TAILQ_FOREACH(vq, &d->vdev.vqs, next)
			rc |= virtqueue_ring_interrupt(vq);
		return rc;
	} else if (d->modern) {
		isr_status = virtio_mmio_cread8((void *)(__uptr)d->pci_isr_addr,
					       0);
	} else {
		isr_status = virtio_cread8((void *)(__uptr)d->pci_isr_addr, 0);
	}

	if (isr_status & VIRTIO_PCI_ISR_CONFIG) {
		/* We don't support configuration interrupt on the device */
		uk_pr_warn_isr("Unsupported config change interrupt received on virtio-pci device %p\n",
			       d);
	}

	if (isr_status & VIRTIO_PCI_ISR_HAS_INTR)
		UK_TAILQ_FOREACH(vq, &d->vdev.vqs, next)
			rc |= virtqueue_ring_interrupt(vq);

	return rc;
}

static struct virtqueue *vpci_legacy_vq_setup(struct virtio_dev *vdev,
					      __u16 queue_id,
					      __u16 num_desc,
					      virtqueue_callback_t callback,
					      struct uk_alloc *a)
{
	struct virtio_pci_dev *vpdev = NULL;
	struct virtqueue *vq;
	__paddr_t addr;
	long flags;

	UK_ASSERT(vdev != NULL);

	vpdev = to_virtiopcidev(vdev);
	vq = virtqueue_create(queue_id, num_desc, VIRTIO_PCI_VRING_ALIGN,
			      callback, vpci_legacy_notify, vdev, a);
	if (PTRISERR(vq)) {
		uk_pr_err("Failed to create the virtqueue: %d\n",
			  PTR2ERR(vq));
		goto err_exit;
	}

	/* Physical address of the queue */
	addr = virtqueue_physaddr(vq);
	/* Select the queue of interest */
	virtio_cwrite16((void *)(unsigned long)vpdev->pci_base_addr,
			VIRTIO_PCI_QUEUE_SEL, queue_id);
	virtio_cwrite32((void *)(unsigned long)vpdev->pci_base_addr,
			VIRTIO_PCI_QUEUE_PFN,
			addr >> VIRTIO_PCI_QUEUE_ADDR_SHIFT);

	flags = uk_lcpu_save_irqf();
	UK_TAILQ_INSERT_TAIL(&vpdev->vdev.vqs, vq, next);
	uk_lcpu_restore_irqf(flags);

err_exit:
	return vq;
}

static void vpci_legacy_vq_release(struct virtio_dev *vdev,
		struct virtqueue *vq, struct uk_alloc *a)
{
	struct virtio_pci_dev *vpdev = NULL;
	long flags;

	UK_ASSERT(vq != NULL);
	UK_ASSERT(a != NULL);
	vpdev = to_virtiopcidev(vdev);

	/* Select and deactivate the queue */
	virtio_cwrite16((void *)(unsigned long)vpdev->pci_base_addr,
			VIRTIO_PCI_QUEUE_SEL, vq->queue_id);
	virtio_cwrite32((void *)(unsigned long)vpdev->pci_base_addr,
			VIRTIO_PCI_QUEUE_PFN, 0);

	flags = uk_lcpu_save_irqf();
	UK_TAILQ_REMOVE(&vpdev->vdev.vqs, vq, next);
	uk_lcpu_restore_irqf(flags);

	virtqueue_destroy(VIRTIO_PCI_VRING_ALIGN, vq, a);
}

static int vpci_legacy_pci_vq_find(struct virtio_dev *vdev, __u16 num_vqs,
				   __u16 *qdesc_size)
{
	struct virtio_pci_dev *vpdev = NULL;
	int vq_cnt = 0, i = 0, rc = 0;

	UK_ASSERT(vdev);
	vpdev = to_virtiopcidev(vdev);

	/* Registering the interrupt for the queue */
	rc = uk_intctlr_irq_register(vpdev->pdev->irq, virtio_pci_handle,
				     vpdev);
	if (rc != 0) {
		uk_pr_err("Failed to register the interrupt\n");
		return rc;
	}

	for (i = 0; i < num_vqs; i++) {
		virtio_cwrite16((void *) (unsigned long)vpdev->pci_base_addr,
				VIRTIO_PCI_QUEUE_SEL, i);
		qdesc_size[i] = virtio_cread16(
				(void *) (unsigned long)vpdev->pci_base_addr,
				VIRTIO_PCI_QUEUE_SIZE);
		if (unlikely(!qdesc_size[i])) {
			uk_pr_err("Virtqueue %d not available\n", i);
			continue;
		}
		vq_cnt++;
	}
	return vq_cnt;
}

static int vpci_legacy_pci_config_set(struct virtio_dev *vdev, __u16 offset,
				      const void *buf, __u32 len)
{
	struct virtio_pci_dev *vpdev = NULL;

	UK_ASSERT(vdev);
	vpdev = to_virtiopcidev(vdev);

	virtio_cwrite_bytes((void *)(unsigned long)vpdev->pci_base_addr,
			    VIRTIO_PCI_CONFIG_OFF + offset, buf, len, 1);

	return 0;
}

static int vpci_legacy_pci_config_get(struct virtio_dev *vdev, __u16 offset,
				      void *buf, __u32 len, __u8 type_len)
{
	struct virtio_pci_dev *vpdev = NULL;
	int rc = 0;

	UK_ASSERT(vdev);
	vpdev = to_virtiopcidev(vdev);

	/* Reading an entity less than 4 bytes are atomic */
	if (type_len == len && type_len <= 4) {
		virtio_cread_bytes(
				(void *) (unsigned long)vpdev->pci_base_addr,
				VIRTIO_PCI_CONFIG_OFF + offset, buf, len,
				type_len);
	} else {
		__u32 len_bytes;

		if (__builtin_umul_overflow(len, type_len, &len_bytes))
			return -EFAULT;

		rc = virtio_cread_bytes_many(
				(void *) (unsigned long)vpdev->pci_base_addr,
				VIRTIO_PCI_CONFIG_OFF + offset,	buf, len_bytes);
		if (unlikely(rc != (int) len_bytes))
			return -EFAULT;
	}

	return 0;
}

static __u8 vpci_legacy_pci_status_get(struct virtio_dev *vdev)
{
	struct virtio_pci_dev *vpdev = NULL;

	UK_ASSERT(vdev);
	vpdev = to_virtiopcidev(vdev);
	return virtio_cread8((void *) (unsigned long) vpdev->pci_base_addr,
			     VIRTIO_PCI_STATUS);
}

static void vpci_legacy_pci_status_set(struct virtio_dev *vdev, __u8 status)
{
	struct virtio_pci_dev *vpdev = NULL;
	__u8 curr_status = 0;

	/* Reset should be performed using the reset interface */
	UK_ASSERT(vdev || status != VIRTIO_CONFIG_STATUS_RESET);

	vpdev = to_virtiopcidev(vdev);
	curr_status = vpci_legacy_pci_status_get(vdev);
	status |= curr_status;
	virtio_cwrite8((void *)(unsigned long) vpdev->pci_base_addr,
		       VIRTIO_PCI_STATUS, status);
}

static void vpci_legacy_pci_dev_reset(struct virtio_dev *vdev)
{
	struct virtio_pci_dev *vpdev = NULL;
	__u8 status;

	UK_ASSERT(vdev);

	vpdev = to_virtiopcidev(vdev);
	/**
	 * Resetting the device.
	 */
	virtio_cwrite8((void *) (unsigned long)vpdev->pci_base_addr,
		       VIRTIO_PCI_STATUS, VIRTIO_CONFIG_STATUS_RESET);
	/**
	 * Waiting for the resetting the device. Find a better way
	 * of doing this instead of repeating register read.
	 *
	 * NOTE! Spec (4.1.4.3.2)
	 * Need to check if we have to wait for the reset to happen.
	 */
	do {
		status = virtio_cread8(
				(void *)(unsigned long)vpdev->pci_base_addr,
				VIRTIO_PCI_STATUS);
	} while (status != VIRTIO_CONFIG_STATUS_RESET);
}

static __u64 vpci_legacy_pci_features_get(struct virtio_dev *vdev)
{
	struct virtio_pci_dev *vpdev = NULL;
	__u64  features;

	UK_ASSERT(vdev);

	vpdev = to_virtiopcidev(vdev);
	features = virtio_cread32((void *) (unsigned long)vpdev->pci_base_addr,
				  VIRTIO_PCI_HOST_FEATURES);
	return features;
}

static int vpci_modern_pci_config_set(struct virtio_dev *vdev, __u16 offset,
				      const void *buf, __u32 len)
{
	struct virtio_pci_dev *vpdev = to_virtiopcidev(vdev);

	if (!vpdev->device_cfg)
		return -ENODEV;
	virtio_mmio_cwrite_bytes(vpdev->device_cfg, offset, buf, len, 1);
	return 0;
}

static int vpci_modern_pci_config_get(struct virtio_dev *vdev, __u16 offset,
				      void *buf, __u32 len, __u8 type_len)
{
	struct virtio_pci_dev *vpdev = to_virtiopcidev(vdev);
	__u32 len_bytes;

	if (!vpdev->device_cfg)
		return -ENODEV;
	if (__builtin_umul_overflow(len, type_len, &len_bytes))
		return -EFAULT;
	virtio_mmio_cread_bytes(vpdev->device_cfg, offset, buf, len_bytes,
				type_len);
	return 0;
}

static __u8 vpci_modern_pci_status_get(struct virtio_dev *vdev)
{
	struct virtio_pci_dev *vpdev = to_virtiopcidev(vdev);

	return virtio_mmio_cread8(vpdev->common_cfg,
				  VIRTIO_PCI_COMMON_STATUS);
}

static void vpci_modern_pci_status_set(struct virtio_dev *vdev, __u8 status)
{
	struct virtio_pci_dev *vpdev = to_virtiopcidev(vdev);
	__u8 current;

	UK_ASSERT(status != VIRTIO_CONFIG_STATUS_RESET);
	current = vpci_modern_pci_status_get(vdev);
	virtio_mmio_cwrite8(vpdev->common_cfg, VIRTIO_PCI_COMMON_STATUS,
			     current | status);
}

static void vpci_modern_pci_dev_reset(struct virtio_dev *vdev)
{
	struct virtio_pci_dev *vpdev = to_virtiopcidev(vdev);

	virtio_mmio_cwrite8(vpdev->common_cfg, VIRTIO_PCI_COMMON_STATUS,
			     VIRTIO_CONFIG_STATUS_RESET);
	while (vpci_modern_pci_status_get(vdev) != VIRTIO_CONFIG_STATUS_RESET)
		;
}

static __u64 vpci_modern_pci_features_get(struct virtio_dev *vdev)
{
	struct virtio_pci_dev *vpdev = to_virtiopcidev(vdev);
	__u64 features;

	/* Modern virtio splits the 64-bit feature bitmap into two 32-bit pages. */
	virtio_mmio_cwrite32(vpdev->common_cfg, VIRTIO_PCI_COMMON_DFSELECT, 0);
	features = virtio_mmio_cread32(vpdev->common_cfg, VIRTIO_PCI_COMMON_DF);
	virtio_mmio_cwrite32(vpdev->common_cfg, VIRTIO_PCI_COMMON_DFSELECT, 1);
	features |= (__u64)virtio_mmio_cread32(vpdev->common_cfg,
					       VIRTIO_PCI_COMMON_DF) << 32;
	return features;
}

static void vpci_modern_pci_features_set(struct virtio_dev *vdev)
{
	struct virtio_pci_dev *vpdev = to_virtiopcidev(vdev);

	vdev->features = virtqueue_feature_negotiate(vdev->features);
	virtio_mmio_cwrite32(vpdev->common_cfg, VIRTIO_PCI_COMMON_GFSELECT, 0);
	virtio_mmio_cwrite32(vpdev->common_cfg, VIRTIO_PCI_COMMON_GF,
			     (__u32)vdev->features);
	virtio_mmio_cwrite32(vpdev->common_cfg, VIRTIO_PCI_COMMON_GFSELECT, 1);
	virtio_mmio_cwrite32(vpdev->common_cfg, VIRTIO_PCI_COMMON_GF,
			     (__u32)(vdev->features >> 32));
}

static int vpci_modern_pci_vq_find(struct virtio_dev *vdev, __u16 num_vqs,
				   __u16 *qdesc_size)
{
	struct virtio_pci_dev *vpdev = to_virtiopcidev(vdev);
	int vq_count = 0;
	int rc;
	__u16 i;

	rc = vpci_modern_enable_msix(vpdev);
	if (rc) {
		uk_pr_err("Failed to enable virtio-pci MSI-X: %d\n", rc);
		return rc;
	}
	virtio_mmio_cwrite16(vpdev->common_cfg, VIRTIO_PCI_COMMON_MSIX, 0);

	for (i = 0; i < num_vqs; i++) {
		virtio_mmio_cwrite16(vpdev->common_cfg,
				      VIRTIO_PCI_COMMON_QSELECT, i);
		qdesc_size[i] = virtio_mmio_cread16(vpdev->common_cfg,
						VIRTIO_PCI_COMMON_QSIZE);
		if (qdesc_size[i])
			vq_count++;
		else
			uk_pr_err("Virtqueue %u not available\n", i);
	}
	return vq_count;
}

static struct virtqueue *vpci_modern_vq_setup(struct virtio_dev *vdev,
					      __u16 queue_id,
					      __u16 num_desc,
					      virtqueue_callback_t callback,
					      struct uk_alloc *allocator)
{
	struct virtio_pci_dev *vpdev = to_virtiopcidev(vdev);
	struct virtqueue *vq;
	__paddr_t addr;
	long flags;

	vq = virtqueue_create(queue_id, num_desc, VIRTIO_PCI_VRING_ALIGN,
			      callback, vpci_modern_notify, vdev, allocator);
	if (PTRISERR(vq)) {
		uk_pr_err("Failed to create virtqueue: %d\n", PTR2ERR(vq));
		return vq;
	}

	virtio_mmio_cwrite16(vpdev->common_cfg, VIRTIO_PCI_COMMON_QSELECT,
			      queue_id);
	virtio_mmio_cwrite16(vpdev->common_cfg, VIRTIO_PCI_COMMON_QSIZE,
			      num_desc);
	virtio_mmio_cwrite16(vpdev->common_cfg, VIRTIO_PCI_COMMON_QMSIX, 0);

	/*
	 * Cloud Hypervisor compatibility: publish the three split-ring physical
	 * addresses separately, as required by the modern common configuration.
	 */
	addr = virtqueue_physaddr(vq);
	vpci_modern_write64(vpdev->common_cfg, VIRTIO_PCI_COMMON_QDESC, addr);
	addr = virtqueue_get_avail_addr(vq);
	vpci_modern_write64(vpdev->common_cfg, VIRTIO_PCI_COMMON_QDRIVER, addr);
	addr = virtqueue_get_used_addr(vq);
	vpci_modern_write64(vpdev->common_cfg, VIRTIO_PCI_COMMON_QDEVICE, addr);
	virtio_mmio_cwrite16(vpdev->common_cfg, VIRTIO_PCI_COMMON_QENABLE, 1);
	uk_pr_info("Modern virtio-pci queue %u: size=%u desc=%"__PRIx64
		   " avail=%"__PRIx64" used=%"__PRIx64"\n",
		   queue_id, num_desc, (__u64)virtqueue_physaddr(vq),
		   (__u64)virtqueue_get_avail_addr(vq),
		   (__u64)virtqueue_get_used_addr(vq));

	flags = uk_lcpu_save_irqf();
	UK_TAILQ_INSERT_TAIL(&vpdev->vdev.vqs, vq, next);
	uk_lcpu_restore_irqf(flags);
	return vq;
}

static void vpci_modern_vq_release(struct virtio_dev *vdev,
				   struct virtqueue *vq,
				   struct uk_alloc *allocator)
{
	struct virtio_pci_dev *vpdev = to_virtiopcidev(vdev);
	long flags;

	UK_ASSERT(vq);
	UK_ASSERT(allocator);
	virtio_mmio_cwrite16(vpdev->common_cfg, VIRTIO_PCI_COMMON_QSELECT,
			      vq->queue_id);
	virtio_mmio_cwrite16(vpdev->common_cfg, VIRTIO_PCI_COMMON_QENABLE, 0);
	flags = uk_lcpu_save_irqf();
	UK_TAILQ_REMOVE(&vpdev->vdev.vqs, vq, next);
	uk_lcpu_restore_irqf(flags);
	virtqueue_destroy(VIRTIO_PCI_VRING_ALIGN, vq, allocator);
}

static void vpci_legacy_pci_features_set(struct virtio_dev *vdev)
{
	struct virtio_pci_dev *vpdev = NULL;

	UK_ASSERT(vdev);

	vpdev = to_virtiopcidev(vdev);

	/* Mask out features not supported by the virtqueue driver */
	vdev->features = virtqueue_feature_negotiate(vdev->features);

	virtio_cwrite32((void *) (unsigned long)vpdev->pci_base_addr,
			VIRTIO_PCI_GUEST_FEATURES, (__u32)vdev->features);
}

static int virtio_pci_legacy_add_dev(struct pci_device *pci_dev,
				     struct virtio_pci_dev *vpci_dev)
{
	/* Check the valid range of the virtio legacy device */
	if (pci_dev->id.device_id < 0x1000 || pci_dev->id.device_id > 0x103f) {
		uk_pr_err("Invalid Virtio Devices %04x\n",
			  pci_dev->id.device_id);
		return -EINVAL;
	}

	vpci_dev->pci_isr_addr = vpci_dev->pci_base_addr + VIRTIO_PCI_ISR;

	/* Setting the configuration operation */
	vpci_dev->vdev.cops = &vpci_legacy_ops;

	uk_pr_info("Added virtio-pci device %04x\n",
		   pci_dev->id.device_id);
	uk_pr_info("Added virtio-pci subsystem_device_id %04x\n",
		   pci_dev->id.subsystem_device_id);

	/* Mapping the virtio device identifier */
	vpci_dev->vdev.id.virtio_device_id = pci_dev->id.subsystem_device_id;
	return 0;
}

static int virtio_pci_modern_add_dev(struct pci_device *pci_dev,
				     struct virtio_pci_dev *vpci_dev)
{
#if CONFIG_ARCH_X86_64
	__u8 capability;
	__u16 command;
	int remaining = 48;

	if (pci_dev->id.device_id < VIRTIO_PCI_MODERN_DEVICEID_START ||
	    pci_dev->id.device_id > VIRTIO_PCI_MODERN_DEVICEID_END)
		return -EINVAL;
	if (!(vpci_config_read16(pci_dev, 0x06) & PCI_STATUS_CAP_LIST))
		return -ENODEV;

	/*
	 * Cloud Hypervisor compatibility: discover common, notify, ISR, device and
	 * MSI-X regions instead of relying on the legacy PCI I/O BAR layout.
	 */
	capability = vpci_config_read8(pci_dev, 0x34) & ~3U;
	while (capability && remaining--) {
		__u8 cap_id = vpci_config_read8(pci_dev, capability);
		__u8 next = vpci_config_read8(pci_dev, capability + 1);

		if (cap_id == PCI_CAP_ID_MSIX) {
			__u32 table = vpci_config_read32(pci_dev,
						   capability + PCI_MSIX_TABLE);
			__u8 bar = table & PCI_MSIX_TABLE_BIR;
			__u64 bar_base = vpci_bar_base(pci_dev, bar);

			if (bar_base) {
				vpci_dev->msix_capability = capability;
				vpci_dev->msix_table = (void *)(__uptr)
					(VIRTIO_PCI_X86_DIRECTMAP_START + bar_base +
					 (table & PCI_MSIX_TABLE_OFFSET));
			}
		} else if (cap_id == PCI_CAP_ID_VNDR) {
			__u8 cap_len = vpci_config_read8(pci_dev, capability + 2);
			__u8 type = vpci_config_read8(pci_dev,
						   capability +
						   VIRTIO_PCI_CAP_CFG_TYPE);
			__u8 bar = vpci_config_read8(pci_dev,
						  capability + VIRTIO_PCI_CAP_BAR);
			__u32 offset = vpci_config_read32(pci_dev,
						       capability +
						       VIRTIO_PCI_CAP_OFFSET);
			__u64 bar_base = vpci_bar_base(pci_dev, bar);
			void *address = (void *)(__uptr)
				(VIRTIO_PCI_X86_DIRECTMAP_START +
				 bar_base + offset);

			if (cap_len >= 16 && bar_base) {
				switch (type) {
				case VIRTIO_PCI_CAP_COMMON_CFG:
					vpci_dev->common_cfg = address;
					break;
				case VIRTIO_PCI_CAP_NOTIFY_CFG:
					if (cap_len >= 20) {
						vpci_dev->notify_cfg = address;
						vpci_dev->notify_off_multiplier =
							vpci_config_read32(pci_dev,
							 capability +
							 VIRTIO_PCI_NOTIFY_MULTIPLIER);
					}
					break;
				case VIRTIO_PCI_CAP_ISR_CFG:
					vpci_dev->pci_isr_addr = (__uptr)address;
					break;
				case VIRTIO_PCI_CAP_DEVICE_CFG:
					vpci_dev->device_cfg = address;
					break;
				default:
					break;
				}
			}
		}
		capability = next & ~3U;
	}

	if (!vpci_dev->common_cfg || !vpci_dev->notify_cfg ||
	    !vpci_dev->pci_isr_addr || !vpci_dev->notify_off_multiplier) {
		uk_pr_err("Incomplete modern virtio-pci capabilities\n");
		return -ENODEV;
	}

	command = vpci_config_read16(pci_dev, 0x04);
	vpci_config_write16(pci_dev, 0x04, command | 0x0006);
	vpci_dev->modern = 1;
	vpci_dev->vdev.cops = &vpci_modern_ops;
	vpci_dev->vdev.id.virtio_device_id =
		pci_dev->id.device_id - VIRTIO_PCI_MODERN_DEVICEID_START;
	uk_pr_info("Added modern virtio-pci device %04x (type %u)\n",
		   pci_dev->id.device_id,
		   vpci_dev->vdev.id.virtio_device_id);
	uk_pr_info("Modern virtio-pci regions: common=%p notify=%p isr=%p device=%p irq=%lu\n",
		   vpci_dev->common_cfg, vpci_dev->notify_cfg,
		   (void *)(__uptr)vpci_dev->pci_isr_addr,
		   vpci_dev->device_cfg, pci_dev->irq);
	return 0;
#else
	(void)pci_dev;
	(void)vpci_dev;
	return -ENOTSUP;
#endif
}


static int virtio_pci_add_dev(struct pci_device *pci_dev)
{
	struct virtio_pci_dev *vpci_dev = NULL;
	int rc = 0;

	UK_ASSERT(pci_dev != NULL);

	vpci_dev = uk_calloc(a, 1, sizeof(*vpci_dev));
	if (!vpci_dev) {
		uk_pr_err("Failed to allocate virtio-pci device\n");
		return -ENOMEM;
	}

	/* Fetch PCI Device information */
	vpci_dev->pdev = pci_dev;
	vpci_dev->pci_base_addr = pci_dev->base;

	/* Preserve the legacy path for transitional QEMU and Firecracker devices. */
	if (pci_dev->id.device_id >= VIRTIO_PCI_MODERN_DEVICEID_START)
		rc = virtio_pci_modern_add_dev(pci_dev, vpci_dev);
	else
		rc = virtio_pci_legacy_add_dev(pci_dev, vpci_dev);
	if (rc != 0) {
		uk_pr_err("Failed to probe virtio-pci device: %d\n", rc);
		goto free_pci_dev;
	}

	rc = virtio_bus_register_device(&vpci_dev->vdev);
	if (rc != 0) {
		uk_pr_err("Failed to register the virtio device: %d\n", rc);
		goto free_pci_dev;
	}

exit:
	return rc;

free_pci_dev:
	uk_free(a, vpci_dev);
	goto exit;
}

static int virtio_pci_drv_init(struct uk_alloc *drv_allocator)
{
	/* driver initialization */
	if (!drv_allocator)
		return -EINVAL;

	a = drv_allocator;
	return 0;
}

static const struct pci_device_id virtio_pci_ids[] = {
	{PCI_DEVICE_ID(VENDOR_QUMRANET_VIRTIO, PCI_ANY_ID)},
	/* End of Driver List */
	{PCI_ANY_DEVICE_ID},
};

static struct pci_driver virtio_pci_drv = {
	.device_ids = virtio_pci_ids,
	.init = virtio_pci_drv_init,
	.add_dev = virtio_pci_add_dev
};
PCI_REGISTER_DRIVER(&virtio_pci_drv);
