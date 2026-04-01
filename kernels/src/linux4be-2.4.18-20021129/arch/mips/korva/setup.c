/***********************************************************************
 * Copyright 2001 MontaVista Software Inc.
 * Author: Jun Sun, jsun@mvista.com or jsun@junsun.net
 *
 * Markham variant by Geoffrey Espin espin@idiom.com
 *
 * arch/mips/korva/setup.c
 *     The setup file for korva/markham.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 ***********************************************************************
 */

#include <linux/config.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/interrupt.h>
#include <linux/serial.h>
#include <linux/types.h>
#include <linux/string.h>	/* for memset */
#include <linux/ioport.h>

#include <asm/reboot.h>
#include <asm/io.h>
#include <asm/time.h>

#include <asm/korva.h>
#include <asm/debug.h>

#if defined(CONFIG_PCI)
#include "pinot.h"
#endif

#if defined(CONFIG_NEC_MARKHAM_32MB)
#define	USE_CPU_TIMER	1	/* markham uses CPU timer, at least for now */
#endif

extern void nec_korva_restart(char* c);
extern void nec_korva_halt(void);
extern void nec_korva_power_off(void);

void __init bus_error_init(void)
{
}

static void __init init_serial_ports(void)
{
	struct serial_struct s;

	/* clear memory */
	memset(&s, 0, sizeof(s));

	s.line = 0;		 	/* we set the first one */
	s.baud_base = 1152000;
	s.irq = 10;
	s.flags = ASYNC_BOOT_AUTOCONF | ASYNC_SKIP_TEST;
	s.iomem_base = (u8*)(KORVA_BASE_VIRT + KORVA_UARTDLL);
	s.iomem_reg_shift = 2;
	s.io_type = SERIAL_IO_MEM;
	db_verify(early_serial_setup(&s), == 0);
}

extern int setup_irq(unsigned int irq, struct irqaction *irqaction);
static void __init nec_korva_timer_setup(struct irqaction *irq)
{

#if defined(USE_CPU_TIMER)
        unsigned int count;

      	setup_irq(7, irq);

        /* to generate the first CPU timer interrupt */
        count = read_32bit_cp0_register(CP0_COUNT);
        write_32bit_cp0_register(CP0_COMPARE, count + 1000);
#else

        setup_irq(8,irq);

	korva_out32(KORVA_TM0CSR, mips_counter_frequency * 8 / HZ);
	korva_out32(KORVA_TMMR, 0x1);	/* start the timer 0 */
#endif
}


#if defined(CONFIG_BLK_DEV_INITRD)
extern unsigned long __rd_start, __rd_end, initrd_start, initrd_end;
#endif

void __init nec_vr41xx_setup(void)
{
	//MIPS_DEBUG(printk("nec_vr41xx_setup() starts.\n"));

#if defined(CONFIG_BLK_DEV_INITRD)
        ROOT_DEV = MKDEV(RAMDISK_MAJOR, 0);
        initrd_start = (unsigned long)&__rd_start;
        initrd_end = (unsigned long)&__rd_end;
#endif

        board_timer_setup = nec_korva_timer_setup;

	/* figure out the bus clock frequency */
	if (korva_in32(KORVA_S_GSR) & 0x2) {
		/* clock is 66MHz, counter freq is divided by 8 */
        	mips_counter_frequency = 66000000 / 8;
	} else {
        	mips_counter_frequency = 100000000 / 8;
	}

        _machine_restart = nec_korva_restart;
        _machine_halt = nec_korva_halt;
        _machine_power_off = nec_korva_power_off;

	init_serial_ports();

	/* ---------------- board hardware setup ---------------- */
	/* reset all peripherials, except uart */
	/* enable IBUS arbitration for peripherals, UART external clock */
	/* 0x20=pci,0x10=uart,0x8=mac2,0x4=atm,0x2=mac1,0x1=usb */
	korva_out32(KORVA_S_WRCR, 0x2b);

	/* 0x8=ucsel,0x2=iaen */
	korva_set_bits(KORVA_S_GMR, 0x8 | 0x2);

	korva_clear_bits(KORVA_S_IOR, 0xff);    /* LEDs off */

	set_io_port_base(KORVA_BASE_VIRT);	/* 0xb0000000 */

	ioport_resource.start = 0;
	ioport_resource.end = 0xffffffff;

	iomem_resource.start = 0;
	iomem_resource.end = 0xffffffff;

#if defined(CONFIG_PCI)
	{
	volatile u32 tmp;

	*P_SWRR = RSTOUT;	/* reset PINOT */
	udelay(1000);
	*P_SWRR = 0;

	*P_PCI_COMMAND          = (0xffff0000 | PCI_COMMAND_MEMORY | 
				PCI_COMMAND_MASTER | PCI_COMMAND_INVALIDATE |
				PCI_COMMAND_PARITY | PCI_COMMAND_SERR);

	*P_PCI_CLASS_REVISION   = 0x06000000;	/* HOST MODE */
	*P_PCI_CACHE_LINE_SIZE |= 0x04ff0000;	/* 4 words */
	*P_PCI_BASE_ADDRESS_REG = PINOT_REG_BASE_PHYS;	/* (0x14) */

	*P_PCI_BASE_ADDRESS_MEM = PINOT_MEM_START;/* (0x10) 4M at top of mem */
	*P_PCI_PM_DATA_CSR      = 0;		/* ? */

	*P_IBBA                 = PINOT_MEM_START;/* 4M at top of mem */
	*P_PLBA                 = PINOT_IOMEM;	/* b0100000 */

	*P_BCNT                 = 0x80000020;	/* INITD + ICMDS */

	tmp = *P_IGSR;	/* clear IBUS GSR */
	}
#endif
}

void nec_korva_led (u32 val, u32 mask)
{
	korva_clear_bits(KORVA_S_IOR, (~mask));
	korva_set_bits(KORVA_S_IOR, (val & mask));
}
