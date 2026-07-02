/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026, Unikraft. */

#include <string.h>
#include <uk/essentials.h>
#include <uk/arch/types.h>
#include <uk/paging.h>
#include <uk/pm.h>
#include <uk/plat/common/bootinfo.h>
#include <uk/plat/common/memory.h>
#include <uk/plat/common/sections.h>

#define pvh_crash(rc, msg, ...) uk_pm_syscrash()

#define XEN_HVM_START_MAGIC_VALUE	0x336ec578
#define XEN_HVM_MEMMAP_TYPE_RAM		1

struct hvm_start_info {
	__u32 magic;
	__u32 version;
	__u32 flags;
	__u32 nr_modules;
	__u64 modlist_paddr;
	__u64 cmdline_paddr;
	__u64 rsdp_paddr;
	__u64 memmap_paddr;
	__u32 memmap_entries;
	__u32 reserved;
} __packed;

struct hvm_modlist_entry {
	__u64 paddr;
	__u64 size;
	__u64 cmdline_paddr;
	__u64 reserved;
} __packed;

struct hvm_memmap_table_entry {
	__u64 addr;
	__u64 size;
	__u32 type;
	__u32 reserved;
} __packed;

static void pvh_init_cmdline(struct ukplat_bootinfo *bi,
			     const struct hvm_start_info *si)
{
	const char *cmdline;

	if (!si->cmdline_paddr)
		return;

	cmdline = (const char *)(__uptr)si->cmdline_paddr;
	bi->cmdline = (__u64)(__uptr)cmdline;
	bi->cmdline_len = strlen(cmdline);
}

static void pvh_init_initrd(struct ukplat_bootinfo *bi,
			    const struct hvm_start_info *si)
{
	const struct hvm_modlist_entry *mods;
	struct ukplat_memregion_desc mrd = {0};
	const struct hvm_modlist_entry *mod;
	int rc;

	if (!si->nr_modules || !si->modlist_paddr)
		return;

	mods = (const struct hvm_modlist_entry *)(__uptr)si->modlist_paddr;
	mod = &mods[0];
	if (!mod->paddr || !mod->size)
		return;

	mrd.pbase = UK_PAGING_PAGE_ALIGN_DOWN(mod->paddr);
	mrd.vbase = mrd.pbase;
	mrd.pg_off = mod->paddr - mrd.pbase;
	mrd.len = mod->size;
	mrd.type = UKPLAT_MEMRT_INITRD;
	mrd.pg_count = UK_PAGING_PAGE_COUNT(mrd.pg_off + mod->size);
	mrd.flags = UKPLAT_MEMRF_READ;
#ifdef CONFIG_UKPLAT_MEMRNAME
	memcpy(mrd.name, "initrd", sizeof("initrd"));
#endif

	rc = ukplat_memregion_list_insert(&bi->mrds, &mrd);
	if (unlikely(rc < 0))
		pvh_crash(rc, "Unable to add initrd mapping");
}

static void pvh_init_mem(struct ukplat_bootinfo *bi,
			 const struct hvm_start_info *si)
{
	const struct hvm_memmap_table_entry *e820;
	struct ukplat_memregion_desc mrd = {0};
	__u64 start, end;
	__u32 i;
	int rc;

	/* Keep ELF headers and the PVH note out of the free-memory allocator. */
	start = UK_PAGING_PAGE_ALIGN_DOWN(__BASE_ADDR);
	end = UK_PAGING_PAGE_ALIGN_UP(__TEXT);
	if (end > start) {
		mrd.pbase = start;
		mrd.vbase = start;
		mrd.len = end - start;
		mrd.pg_count = UK_PAGING_PAGE_COUNT(mrd.len);
		mrd.type = UKPLAT_MEMRT_RESERVED;
		mrd.flags = UKPLAT_MEMRF_READ;
#ifdef CONFIG_UKPLAT_MEMRNAME
		memcpy(mrd.name, "img-gap", sizeof("img-gap"));
#endif
		rc = ukplat_memregion_list_insert(&bi->mrds, &mrd);
		if (unlikely(rc < 0))
			pvh_crash(rc, "Unable to reserve image gap");
	}

	if (unlikely(!si->memmap_entries || !si->memmap_paddr))
		pvh_crash(-EINVAL, "Missing PVH memory map");

	e820 = (const struct hvm_memmap_table_entry *)(__uptr)si->memmap_paddr;
	for (i = 0; i < si->memmap_entries; i++) {
		start = MAX(e820[i].addr, UK_PAGING_PAGE_SIZE);
		end = e820[i].addr + e820[i].size;
		if (end <= start)
			continue;

		mrd.pbase = UK_PAGING_PAGE_ALIGN_DOWN(start);
		mrd.vbase = mrd.pbase;
		mrd.pg_off = start - mrd.pbase;
		mrd.len = end - start;
		mrd.pg_count = UK_PAGING_PAGE_COUNT(mrd.pg_off + mrd.len);

		if (e820[i].type == XEN_HVM_MEMMAP_TYPE_RAM) {
			mrd.type = UKPLAT_MEMRT_FREE;
			mrd.flags = UKPLAT_MEMRF_READ | UKPLAT_MEMRF_WRITE;
			mrd.len = UK_PAGING_PAGE_ALIGN_UP(mrd.len + mrd.pg_off);
		} else {
			mrd.type = UKPLAT_MEMRT_RESERVED;
			mrd.flags = UKPLAT_MEMRF_READ;
		}

		rc = ukplat_memregion_list_insert(&bi->mrds, &mrd);
		if (unlikely(rc < 0))
			pvh_crash(rc, "Unable to add memory map");
	}

	rc = ukplat_memregion_list_insert_legacy_hi_mem(&bi->mrds);
	if (unlikely(rc < 0))
		pvh_crash(rc, "Failed to insert legacy high memory region");
}

void _ukplat_entry(struct ukplat_bootinfo *bi);

void pvh_entry(struct hvm_start_info *si)
{
	struct ukplat_bootinfo *bi;

	if (unlikely(si->magic != XEN_HVM_START_MAGIC_VALUE))
		pvh_crash(-EINVAL, "Invalid PVH magic");

	bi = ukplat_bootinfo_get();
	if (unlikely(!bi))
		pvh_crash(-EINVAL, "Incompatible or corrupted bootinfo");

	pvh_init_cmdline(bi, si);
	pvh_init_initrd(bi, si);
	pvh_init_mem(bi, si);
	memcpy(bi->bootprotocol, "pvh", sizeof("pvh"));

	_ukplat_entry(bi);
}
