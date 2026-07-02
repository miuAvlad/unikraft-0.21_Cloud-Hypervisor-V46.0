/* SPDX-License-Identifier: ISC */
/*
 * Original author(s): Dan Williams <djwillia@us.ibm.com>
 *                     Martin Lucina <martin.lucina@docker.com>
 *                     Simon Kuenzer <simon.kuenzer@neclab.eu>
 * PVH and Cloud Hypervisor adaptation: miuAvlad, 2026
 *
 * Copyright (c) 2016, IBM
 *           (c) 2016-2017 Docker, Inc.
 *           (c) 2017-2018, NEC Europe Ltd.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear
 * in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <uk/arch/limits.h>
#include <uk/plat/common/common.lds.h>

#define BASE_ADDR 0x100000

PHDRS
{
#if CONFIG_ELF_PHDRS_IN_PT_LOAD
	phdrs PT_PHDR PHDRS;
	headers PT_LOAD PHDRS FILEHDR FLAGS(PHDRS_PF_R);
#endif
	note PT_NOTE FLAGS(0);
	text PT_LOAD FLAGS(PHDRS_PF_RX);
	rodata PT_LOAD FLAGS(PHDRS_PF_R);
	data PT_LOAD;
	tls PT_TLS;
	tls_load PT_LOAD;
	stack PT_GNU_STACK FLAGS(PHDRS_PF_RW);
}

SECTIONS
{
#if CONFIG_ELF_PHDRS_IN_PT_LOAD
	PROVIDE(__executable_start = BASE_ADDR);
	. = BASE_ADDR + SIZEOF_HEADERS;
	PROVIDE(__phdr = .);
#else
	. = BASE_ADDR;
#endif
	_base_addr = BASE_ADDR;

#if CONFIG_ELF_PHDRS_IN_PT_LOAD
	.headers :
	{
		KEEP (*(.data.boot))
		KEEP (*(.data.boot.*))
	} :headers
#endif

	.notes :
	{
		KEEP (*(.note.Xen))
	} :note

	_text = .;
	.text :
	{
#if !CONFIG_ELF_PHDRS_IN_PT_LOAD
		KEEP (*(.data.boot))
		KEEP (*(.data.boot.*))
#endif
		*(.text.boot)
		*(.text.boot.*)
		*(.text)
		*(.text.*)
	} :text
	_etext = .;

	. = ALIGN(__PAGE_SIZE);
	_rodata = .;
	.rodata :
	{
		*(.rodata)
		*(.rodata.*)
	} :rodata
	_erodata = .;

	EXCEPTION_SECTIONS
	CTORTAB_SECTION
	INITTAB_SECTION
	EVENTTAB_SECTION

	. = ALIGN(0x8);
	_ctors = .;
	.preinit_array : {
		PROVIDE_HIDDEN (__preinit_array_start = .);
		KEEP (*(.preinit_array))
		PROVIDE_HIDDEN (__preinit_array_end = .);
	}

	. = ALIGN(0x8);
	.init_array : {
		PROVIDE_HIDDEN (__init_array_start = .);
		KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
		KEEP (*(.init_array .ctors))
		PROVIDE_HIDDEN (__init_array_end = .);
	}
	_ectors = .;

	TLS_SECTIONS
	DATA_SECTIONS

	_end = .;

	.comment 0 : { *(.comment) }
	DEBUG_SYMBOLS
	DISCARDS
}
