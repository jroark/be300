/*
 * Setup pointers to hardware dependent routines.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1996, 1997 by Ralf Baechle
 * Copyright (C) 2001 by Liam Davies (ldavies@agile.tv)
 *
 */

#include <linux/config.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/mc146818rtc.h>
#include <linux/init.h>
#include <linux/ide.h>

#include <asm/bootinfo.h>
#include <asm/time.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/cobalt/cobalt.h>
#include <asm/pci.h>
#include <asm/processor.h>
#include <asm/reboot.h>

extern void cobalt_machine_restart(char *command);
extern void cobalt_machine_halt(void);
extern void cobalt_machine_power_off(void);
extern void rtc_dallas_init(unsigned long base);
extern struct ide_ops std_ide_ops;

char arcs_cmdline[CL_SIZE] = { "console=ttyS0,115200 root=/dev/hda1" };

void __init bus_error_init(void)
{
}

const char *get_system_type(void)
{
	return "MIPS Cobalt";
}

#define GALILEO_T0_VAL         0xb4000850
#define GALILEO_TIMER_CTRL     0xb4000864
#define GALILEO_CPU_MASK       0xb4000c1c

#define GALILEO_ENTC0          0x01
#define GALILEO_SELTC0         0x02

static void __init cobalt_timer_setup(struct irqaction *irq)
{
	/* Load timer value for 150 Hz */
	volatile unsigned long *timer_reg = (volatile unsigned long *)GALILEO_T0_VAL;

	*timer_reg = 500000;

	/* Register our timer interrupt */
	setup_irq(0, irq);

	/* Enable timer ints */
	*((volatile unsigned long *) GALILEO_TIMER_CTRL) =
			(unsigned long) (GALILEO_ENTC0 | GALILEO_SELTC0);
	/* Unmask timer int */
	*((volatile unsigned long *) GALILEO_CPU_MASK) = (unsigned long) 0x00000100;
}

static void __init cobalt_time_init(void)
{
	/* we have ds1396 RTC chip */
	rtc_dallas_init(VIA_CMOS_ADDR);

	/* optional: we don't have a good way to set RTC time,
	 * so we will hack here to set a time.  In normal running.
	 * it should *not* be called becaues RTC will keep the correct time.
	 */
	/* rtc_set_time(mktime(2001, 10, 05, 17, 20, 0)); */
}

int cobalt_serial_present;
int cobalt_serial_type;
int cobalt_is_raq;

void __init cobalt_setup(void)
{
	/*
 	 * IO/MEM resources.
 	 */

#ifdef CONFIG_NEW_PCI
        ioport_resource.start = 0x10000000;
        ioport_resource.end = 0x11FFFFFF;
        iomem_resource.start = 0x12000000;
        iomem_resource.end = 0x13FFFFFF;
#else
        ioport_resource.start = 0x00000000;
        ioport_resource.end = 0xFFFFFFFF;
#endif
	_machine_restart = cobalt_machine_restart;
	_machine_halt = cobalt_machine_halt;
	_machine_power_off = cobalt_machine_power_off;

	board_time_init = cobalt_time_init;
	board_timer_setup = cobalt_timer_setup;       

#ifdef CONFIG_BLK_DEV_IDE
	ide_ops = &std_ide_ops;
#endif
        set_io_port_base(0xb0000000);

	/* 
	 * This is a prom style console. We just poke at the
	 *  UART to make it talk.
	 * Only use this console if you really screw up and can't
	 *  get to the stage of setting up a real serial console.
	 */
	/*ns16550_setup_console();*/

	/* We have to do this early, here, before the value could
	 * possibly be overwritten by the bootup sequence.
	 */
	cobalt_serial_present = *((unsigned long *) 0xa020001c);
	cobalt_serial_type    = *((unsigned long *) 0xa0200020);
	cobalt_is_raq         = (cobalt_serial_present != 0x0
					&& cobalt_serial_type == 0x1);
}

/* Prom init. We read our one and only communication with the
    firmware. Grab the amount of installed memory */
void __init prom_init(int argc)
{
	mips_machgroup = MACH_GROUP_COBALT;

	add_memory_region(0x0, argc & 0x7fffffff, BOOT_MEM_RAM);
}

void __init prom_free_prom_memory(void)
{
	/* Nothing to do! */
}
