/*
 * FILE NAME
 *	arch/mips/vr41xx/vr4121/workpad/init.c
 *
 * BRIEF MODULE DESCRIPTION
 *	Initialisation code for the IBM WorkPad z50.
 *
 * Copyright 2002 Yoichi Yuasa
 *                yuasa@hh.iij4u.or.jp
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or (at your
 *  option) any later version.
 */
#include <linux/config.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include <asm/bootinfo.h>

char arcs_cmdline[CL_SIZE];

const char *get_system_type(void)
{
	return "IBM WorkPad z50";
}

void __init bus_error_init(void)
{
}

void __init prom_init(int argc, char **argv, unsigned long magic, int *prom_vec)
{
	int mem_size = CONFIG_IBM_WORKPAD_MEM_SIZE;
	int i;

	/*
	 * collect args and prepare cmd_line
	 */
	for (i = 1; i < argc; i++) {
		strcat(arcs_cmdline, argv[i]);
		if (i < (argc - 1))
			strcat(arcs_cmdline, " ");
	}

#if defined(CONFIG_SERIAL_CONSOLE)
	/* to use 9600 ttyS0 serial console */
	strcat(arcs_cmdline, " console=ttyS0,9600");
#endif
#ifdef CONFIG_BLK_DEV_IDEDISK
	strcat(arcs_cmdline, " root=/dev/hdc1");
#endif

	mips_machgroup = MACH_GROUP_NEC_VR41XX;
	mips_machtype = MACH_IBM_WORKPAD;

	/* Add memory region */
	switch (mem_size) {
	case 16:
		add_memory_region(0,  16 << 20, BOOT_MEM_RAM);
		break;
	case 32:
		add_memory_region(0,  32 << 20, BOOT_MEM_RAM);
		break;
	case 48:
		add_memory_region(0,  16 << 20, BOOT_MEM_RAM);
		add_memory_region(32 << 20,  64 << 20, BOOT_MEM_RAM);
		break;
	case 64:
		add_memory_region(0,  64 << 20, BOOT_MEM_RAM);
		break;
	default:
		panic("Memory size error");
		break;
	}
}

void __init prom_free_prom_memory (void)
{
}
