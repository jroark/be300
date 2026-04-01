/***********************************************************************
 * Copyright 2001 MontaVista Software Inc.
 * Author: Jun Sun, jsun@mvista.com or jsun@junsun.net
 *
 * Modified by Geoffrey Espin espin@idiom.com for Markham variant.
 *
 * arch/mips/korva/prom.c
 *     prom setup file for korva
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 ***********************************************************************
 */

#include <linux/init.h>
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/bootmem.h>

#include <asm/bootinfo.h>
#include <asm/addrspace.h>

#include <asm/korva.h>

char arcs_cmdline[CL_SIZE];
const char *get_system_type(void)
{
        return "NEC_Vr41xx Korva";
}

void __init prom_init(void)
{
	u32 dram_size;
	u32 reserve = 0;
	u32 sdtsr;
	u32 ramsize;

#ifdef CONFIG_SERIAL_CONSOLE
        strcpy(arcs_cmdline, "console=ttyS0,115200");
#endif

        mips_machgroup = MACH_GROUP_NEC_VR41XX;
        mips_machtype = MACH_NEC_KORVA;

	/* bit 8:9 determines the RAM size */
	sdtsr = korva_in32(KORVA_SDTSR);
	ramsize =  2 << (1 + ((sdtsr >> 8) & 3));

#if defined(CONFIG_PCI)
	/* 
	 * the PCI window onto 0x01c00000 is at 0x10100000;
	 * on MIPS 0xb0000000 is 0x10000000 (+KSEG1 uncached).
	 */

	reserve = 4;	/* last 4M reserved for PCI; see pci-dma.c & pinot.h */
	add_memory_region((ramsize-reserve) << 20, reserve << 20, BOOT_MEM_RESERVED+0);
#endif

	printk("\n\nKorva%s %dMB @%dMHz\n",
			(reserve == 0) ? "" : "-Markham PCI", ramsize,
			((korva_in32 (KORVA_S_GSR) & 2) ? 66 : 100));

	dram_size = ramsize - reserve;

	add_memory_region(0, dram_size << 20, BOOT_MEM_RAM);

#ifdef	NEC_KORVA_FLASH_8M
	add_memory_region(0xbf800000, 8 << 20, BOOT_MEM_ROM_DATA);
#else
	add_memory_region(0xbfc00000, 4 << 20, BOOT_MEM_ROM_DATA);
#endif
}

void __init prom_free_prom_memory(void)
{
}

void __init prom_fixup_mem_map(unsigned long start, unsigned long end)
{
}
