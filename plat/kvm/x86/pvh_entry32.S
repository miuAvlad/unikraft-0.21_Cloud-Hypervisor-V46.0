/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026, Unikraft. */

#include <uk/config.h>
#include <uk/asm.h>
#include <uk/reloc.h>
#include <uk/lcpu/start.h>

#define XEN_ELFNOTE_PHYS32_ENTRY 18
#define XEN_HVM_START_MAGIC_VALUE 0x336ec578

#define ELFNOTE(name, type, desc)           \
	.pushsection .note.name               ; \
	.align 4                              ; \
	.long 2f - 1f       /* namesz */      ; \
	.long 4f - 3f       /* descsz */      ; \
	.long type          /* type   */      ; \
1:.asciz #name          /* name   */      ; \
2:.align 4                                ; \
3:desc                  /* desc   */      ; \
4:.align 4                                ; \
	.popsection

ELFNOTE(Xen, XEN_ELFNOTE_PHYS32_ENTRY, .long _pvh32_entry)

.section .bss
.space 4096
pvh_bootstack:

/**
 * 32-bit PVH entry function
 *
 * EBX holds the physical address of struct hvm_start_info. The CPU starts in
 * 32-bit protected mode with paging disabled. Hand off to lcpu_start32 to
 * enter long mode.
 */
.code32
.section .text.32.boot
ENTRY(_pvh32_entry)
	cmpl	$XEN_HVM_START_MAGIC_VALUE, (%ebx)
	jne	no_pvh

	/* Relocate PIE images before accessing relocation-aware pointers. */
	movl	$0x00100000, %edi
	movl	$0x00000000, %esi
	movl	$0x00100000, %edx
	do_uk_reloc32	0

	ur_mov	pvh_bootstack, %edi, 4, _phys
	ur_mov	uk_plat_native_sstackp, %esi, 4, _phys
	movl	%edi, (%esi)

	ur_mov	pvh_entry, %edi, 4, _phys
	ur_mov	uk_plat_native_sentry, %esi, 4, _phys
	movl	%edi, (%esi)

	ur_mov	uk_plat_native_sarg, %esi, 4, _phys
	movl	%ebx, (%esi)

	ur_mov	lcpu_start32, %ebx, 4, _phys
	jmp	*%ebx

no_pvh:
	cli
1:
	hlt
	jmp	1b
END(_pvh32_entry)

.section .note.GNU-stack,"",@progbits
