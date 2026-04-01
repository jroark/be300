/*
 * arch/mips/mm/pg-vr4131.c
 *
 * MMU/Cache routines for early revisions of the NEC Vr4131 processor.
 *
 * Copyright (C) 2001, 2002 Paul Mundt <lethal@chaoticdreams.org>
 * Copyright (C) 2001 NEC Electronics
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */
#include <linux/sched.h>
#include <linux/mm.h>

#include <asm/cacheops.h>

void r4k_clear_page_d16(void *page)
{
	unsigned long reg1;

	__asm__ __volatile__(
		".set\tnoreorder\n\t"
		".set\tnoat\n\t"
		".set\tmips3\n\t"
		"mfc0 %1,$16\n\t"
		"nop\n\t"
		"mtc0\t$0,$28\n\t"
		"mtc0\t$0,$29\n\t"
		"nop\n\t"	
		"daddiu\t$1,%0,%3\n"
		"1:\tcache\t0x09,(%0)\n\t"
		"cache\t%4,(%0)\n\t"
		"sd\t$0,(%0)\n\t"
		"sd\t$0,8(%0)\n\t"
		"cache\t0x09,(%0)\n\t"
		"cache\t0x09,16(%0)\n\t"
		"cache\t%4,16(%0)\n\t"
		"sd\t$0,16(%0)\n\t"
		"sd\t$0,24(%0)\n\t"
		"daddiu\t%0,64\n\t"
		"cache\t0x09,16(%0)\n\t"
		"cache\t0x09,-32(%0)\n\t"
		"cache\t%4,-32(%0)\n\t"
		"sd\t$0,-32(%0)\n\t"
		"sd\t$0,-24(%0)\n\t"
		"cache\t0x09,-32(%0)\n\t"
		"cache\t0x09,-16(%0)\n\t"
		"cache\t%4,-16(%0)\n\t"
		"sd\t$0,-16(%0)\n\t"
		"sd\t$0,-8(%0)\n\t"
		"cache\t0x09,-16(%0)\n\t"
		"nop\n\t"
		"bne\t$1,%0,1b\n\t"
		"nop\n\t"
		"mtc0 %1,$16\n\t"
		"nop\n\t"
		".set\tmips0\n\t"
		".set\tat\n\t"
		".set\treorder"
		:"=r" (page), "=&r" (reg1)
		:"0" (page),
		 "I" (PAGE_SIZE),
		 "i" (Index_Writeback_Inv_D)
		:"$1", "memory");
}

void r4k_copy_page_d16(void *to, void *from)
{
	unsigned long dummy1, dummy2;
	unsigned long reg1, reg2, reg3, reg4;

	__asm__ __volatile__(
		".set\tnoreorder\n\t"
		".set\tnoat\n\t"
		".set\tmips3\n\t"
		"daddiu\t$1,%0,%8\n"
		"1:\tcache\t%9,(%0)\n\t"
		"lw\t%2,(%1)\n\t"
		"lw\t%3,4(%1)\n\t"
		"lw\t%4,8(%1)\n\t"
		"lw\t%5,12(%1)\n\t"
		"sw\t%2,(%0)\n\t"
		"sw\t%3,4(%0)\n\t"
		"sw\t%4,8(%0)\n\t"
		"sw\t%5,12(%0)\n\t"
		"cache\t%9,16(%0)\n\t"
		"lw\t%2,16(%1)\n\t"
		"lw\t%3,20(%1)\n\t"
		"lw\t%4,24(%1)\n\t"
		"lw\t%5,28(%1)\n\t"
		"sw\t%2,16(%0)\n\t"
		"sw\t%3,20(%0)\n\t"
		"sw\t%4,24(%0)\n\t"
		"sw\t%5,28(%0)\n\t"
		"cache\t%9,32(%0)\n\t"
		"daddiu\t%0,64\n\t"
		"daddiu\t%1,64\n\t"
		"lw\t%2,-32(%1)\n\t"
		"lw\t%3,-28(%1)\n\t"
		"lw\t%4,-24(%1)\n\t"
		"lw\t%5,-20(%1)\n\t"
		"sw\t%2,-32(%0)\n\t"
		"sw\t%3,-28(%0)\n\t"
		"sw\t%4,-24(%0)\n\t"
		"sw\t%5,-20(%0)\n\t"
		"cache\t%9,-16(%0)\n\t"
		"lw\t%2,-16(%1)\n\t"
		"lw\t%3,-12(%1)\n\t"
		"lw\t%4,-8(%1)\n\t"
		"lw\t%5,-4(%1)\n\t"
		"sw\t%2,-16(%0)\n\t"
		"sw\t%3,-12(%0)\n\t"
		"sw\t%4,-8(%0)\n\t"
		"bne\t$1,%0,1b\n\t"
		"sw\t%5,-4(%0)\n\t"
		".set\tmips0\n\t"
		".set\tat\n\t"
		".set\treorder"
		:"=r" (dummy1), "=r" (dummy2),
		 "=&r" (reg1), "=&r" (reg2), "=&r" (reg3), "=&r" (reg4)
		:"0" (to), "1" (from),
		 "I" (PAGE_SIZE),
		 "i" (Index_Writeback_Inv_D));
}

void r4k_clear_page_d32(void *page)
{
	__asm__ __volatile__(
		".set\tnoreorder\n\t"
		".set\tnoat\n\t"
		".set\tmips3\n\t"
		"daddiu\t$1,%0,%2\n"
		"1:\tcache\t%3,(%0)\n\t"
		"sd\t$0,(%0)\n\t"
		"sd\t$0,8(%0)\n\t"
		"sd\t$0,16(%0)\n\t"
		"sd\t$0,24(%0)\n\t"
		"daddiu\t%0,64\n\t"
		"cache\t%3,-32(%0)\n\t"
		"sd\t$0,-32(%0)\n\t"
		"sd\t$0,-24(%0)\n\t"
		"sd\t$0,-16(%0)\n\t"
		"bne\t$1,%0,1b\n\t"
		"sd\t$0,-8(%0)\n\t"
		".set\tmips0\n\t"
		".set\tat\n\t"
		".set\treorder"
		:"=r" (page)
		:"0" (page),
		 "I" (PAGE_SIZE),
		 "i" (Index_Writeback_Inv_D)
		:"$1","memory");
}

void r4k_copy_page_d32(void *to, void *from)
{
	unsigned long dummy1, dummy2;
	unsigned long reg1, reg2, reg3, reg4;

	__asm__ __volatile__(
		".set\tnoreorder\n\t"
		".set\tnoat\n\t"
		".set\tmips3\n\t"
		"daddiu\t$1,%0,%8\n"
		"1:\tcache\t%9,(%0)\n\t"
		"lw\t%2,(%1)\n\t"
		"lw\t%3,4(%1)\n\t"
		"lw\t%4,8(%1)\n\t"
		"lw\t%5,12(%1)\n\t"
		"sw\t%2,(%0)\n\t"
		"sw\t%3,4(%0)\n\t"
		"sw\t%4,8(%0)\n\t"
		"sw\t%5,12(%0)\n\t"
		"lw\t%2,16(%1)\n\t"
		"lw\t%3,20(%1)\n\t"
		"lw\t%4,24(%1)\n\t"
		"lw\t%5,28(%1)\n\t"
		"sw\t%2,16(%0)\n\t"
		"sw\t%3,20(%0)\n\t"
		"sw\t%4,24(%0)\n\t"
		"sw\t%5,28(%0)\n\t"
		"cache\t%9,32(%0)\n\t"
		"daddiu\t%0,64\n\t"
		"daddiu\t%1,64\n\t"
		"lw\t%2,-32(%1)\n\t"
		"lw\t%3,-28(%1)\n\t"
		"lw\t%4,-24(%1)\n\t"
		"lw\t%5,-20(%1)\n\t"
		"sw\t%2,-32(%0)\n\t"
		"sw\t%3,-28(%0)\n\t"
		"sw\t%4,-24(%0)\n\t"
		"sw\t%5,-20(%0)\n\t"
		"lw\t%2,-16(%1)\n\t"
		"lw\t%3,-12(%1)\n\t"
		"lw\t%4,-8(%1)\n\t"
		"lw\t%5,-4(%1)\n\t"
		"sw\t%2,-16(%0)\n\t"
		"sw\t%3,-12(%0)\n\t"
		"sw\t%4,-8(%0)\n\t"
		"bne\t$1,%0,1b\n\t"
		"sw\t%5,-4(%0)\n\t"
		".set\tmips0\n\t"
		".set\tat\n\t"
		".set\treorder"
		:"=r" (dummy1), "=r" (dummy2),
		 "=&r" (reg1), "=&r" (reg2), "=&r" (reg3), "=&r" (reg4)
		:"0" (to), "1" (from),
		 "I" (PAGE_SIZE),
		 "i" (Index_Writeback_Inv_D));
}

void r4k_clear_page_r4600_v1(void *page)
{
}

void r4k_clear_page_r4600_v2(void *page)
{
}

void r4k_clear_page_s16(void *page)
{
}

void r4k_clear_page_s32(void *page)
{
}

void r4k_clear_page_s64(void *page)
{
}

void r4k_clear_page_s128(void *page)
{
}

void r4k_copy_page_r4600_v1(void *to, void *from)
{
}

void r4k_copy_page_r4600_v2(void *to, void *from)
{
}

void r4k_copy_page_s16(void *to, void *from)
{
}

void r4k_copy_page_s32(void *to, void *from)
{
}

void r4k_copy_page_s64(void *to, void *from)
{
}

void r4k_copy_page_s128(void *to, void *from)
{
}

void pgd_init(unsigned long page)
{
	unsigned long *p = (unsigned long *)page;
	int i;

	for (i = 0; i < USER_PTRS_PER_PGD; i += 8) {
		p[i + 0] = (unsigned long)invalid_pte_table;
		p[i + 1] = (unsigned long)invalid_pte_table;
		p[i + 2] = (unsigned long)invalid_pte_table;
		p[i + 3] = (unsigned long)invalid_pte_table;
		p[i + 4] = (unsigned long)invalid_pte_table;
		p[i + 5] = (unsigned long)invalid_pte_table;
		p[i + 6] = (unsigned long)invalid_pte_table;
		p[i + 7] = (unsigned long)invalid_pte_table;
	}
}

