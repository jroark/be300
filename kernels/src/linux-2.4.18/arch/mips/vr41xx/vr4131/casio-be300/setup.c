/*
 * arch/mips/vr4131/casio-be300/setup.c
 *
 * Setup routines for the Casio Cassiopeia BE-300
 *
 * Copyright (C) 2002 Paul Mundt <lethal@chaoticdreams.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */
#include <linux/init.h>
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/console.h>
#include <linux/ide.h>
#include <linux/ioport.h>

#include <asm/vr41xx.h>
#include <asm/reboot.h>
#include <asm/time.h>

#ifdef CONFIG_BLK_DEV_INITRD
extern unsigned long initrd_start, initrd_end;
extern void *__rd_start, *__rd_end;
#endif

#ifdef CONFIG_BLK_DEV_IDE
extern struct ide_ops be300_ide_ops;
#endif

void __init bus_error_init(void)
{
}

void __init nec_vr41xx_setup(void)
{
	_machine_restart = vr41xx_restart;
	_machine_halt = vr41xx_halt;
	_machine_power_off = vr41xx_power_off;

	board_time_init = vr41xx_time_init;
	board_timer_setup = vr41xx_timer_setup;

	set_io_port_base(0xaa00c000);

#ifdef CONFIG_BLK_DEV_INITRD
	ROOT_DEV = MKDEV(RAMDISK_MAJOR, 0);
	initrd_start = (unsigned long)&__rd_start;
	initrd_end = (unsigned long)&__rd_end;
#endif

#ifdef CONFIG_BLK_DEV_IDE
	ide_ops = &be300_ide_ops;
#endif

#ifdef CONFIG_FB
	conswitchp = &dummy_con;
#endif

	vr41xx_bcu_init();

//	vr41xx_siu_init(1, SIU_RS232C, 0);

#ifdef CONFIG_PCI
	vr41xx_pciu_init();
#endif
}

