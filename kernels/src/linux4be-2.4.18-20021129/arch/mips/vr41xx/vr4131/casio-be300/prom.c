/*
 * arch/mips/vr4131/casio-be300/prom.c
 *
 * PROM library initialization routines for the Casio Cassiopeia BE-300
 *
 * Copyright (C) 2002 Paul Mundt <lethal@chaoticdreams.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include <asm/bootinfo.h>
#include <asm/addrspace.h>
#include <asm/vr41xx.h>

char arcs_cmdline[CL_SIZE];

const char *get_system_type(void)
{
	return "NEC_Vr41xx Casio Cassiopeia BE-300";
}

void __init prom_init(int argc, char **argv, char **envp)
{
	int i;

	for (i = 1; i < argc; i++) {
		strcat(arcs_cmdline, argv[i]);

		if (i < (argc - 1))
			strcat(arcs_cmdline, " ");
	}

	mips_machgroup = MACH_GROUP_NEC_VR41XX;
	mips_machtype = MACH_CASIO_BE300;

	add_memory_region(0, PAGE_ALIGN((16 << 20) - PAGE_SIZE), BOOT_MEM_RAM);
}

void __init prom_free_prom_memory(void)
{
}

