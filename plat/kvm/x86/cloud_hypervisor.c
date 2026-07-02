/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026, Unikraft. */

#include <uk/arch/util.h>
#include <uk/boot/earlytab.h>
#include <uk/pm.h>
#include <uk/prio.h>

__isr static int cloud_hypervisor_exit(void)
{
	/* S5 (5 << 2) combined with the sleep-enable bit (1 << 5). */
	uk_arch_x86_64_outb(0x600, 0x34);
	return -EIO;
}

__isr static int cloud_hypervisor_restart(void)
{
	uk_arch_x86_64_outb(0x600, 0x01);
	return -EIO;
}

static const struct uk_pm_ops cloud_hypervisor_pm_ops = {
	.syshalt = cloud_hypervisor_exit,
	.sysrestart = cloud_hypervisor_restart,
	.syscrash = cloud_hypervisor_exit,
};

static int cloud_hypervisor_register_pm_ops(struct ukplat_bootinfo __unused *bi)
{
	return uk_pm_ops_register(&cloud_hypervisor_pm_ops);
}

UK_BOOT_EARLYTAB_ENTRY(cloud_hypervisor_register_pm_ops, UK_PRIO_EARLIEST);
