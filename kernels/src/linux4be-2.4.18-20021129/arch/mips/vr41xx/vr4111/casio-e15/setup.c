/*
 * FILE NAME
 *	arch/mips/vr41xx/vr4111/casio-e15/setup.c
 *
 * BRIEF MODULE DESCRIPTION
 *	Setup for the CASIO CASSIOPEIA E-15.
 *
 * Copyright 2002 Yoichi Yuasa
 *                yuasa@hh.iij4u.or.jp
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or (at your
 *  option) any later version.
 *
 */
/*
 * Changes:
 *  Francois Leblanc <francois.leblanc@cev-sa.com> Wed, 26 Mar 2002  
 *  - Copy from casio-e55/setup.c an update to casio-e15. 
 *  
 */
#include <linux/config.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/ide.h>
#include <linux/ioport.h>

#include <asm/reboot.h>
#include <asm/power.h>
#include <asm/time.h>
#include <asm/vr41xx.h>
#include <asm/vr41xx/e15.h>

extern void vr41xx_xblink_led(unsigned int times, unsigned int time_on, unsigned int time_off);
extern void vr4111_wait(void);
extern void vr4111_hibernate(void);
extern void vr4111_suspend(void);

extern void vr4111_time_init(void);
extern void vr4111_timer_setup(struct irqaction *irq);

#ifdef CONFIG_BLK_DEV_INITRD
extern unsigned long initrd_start, initrd_end;
extern void * __rd_start, * __rd_end;
#endif

#ifdef CONFIG_BLK_DEV_IDE
extern struct ide_ops e15_ide_ops;
#endif

struct semaphore vr41xx_dma_sem;

#ifdef CONFIG_PM_SUSPEND_WAKEUP
#include <asm/gdb-stub.h>

extern asmlinkage void kernel_entry(void);

extern void breakpoint(void);
extern void DbgInitSerial(void);

// Put contents of WINCE_SIGNATURE_CHECK1 & 2 at 
// WINCE_SIGNATURE_ADD1 & 2 set WINCE_RETURN_ADD 
// to kernel_entry for go back to linux. 

#define WINCE_SIGNATURE_ADD1 	((unsigned int *)0xa0006000)
#define WINCE_SIGNATURE_ADD2 	((unsigned int *)0xa0006004)
#define WINCE_RETURN_ADD 	 	((unsigned int *)0xa0006008)
#define WINCE_SIGNATURE_CHECK1 	((unsigned int *)0x9f00048c)
#define WINCE_SIGNATURE_CHECK2 	((unsigned int *)0x9f000490)

static unsigned int wince_signature_old1;
static unsigned int wince_signature_old2;
static unsigned int wince_return_old;

/* avoid use of sp in this routine since sp isn't correctly set yet*/
static asmlinkage void casio_e15_wakeup(void)
{
	write_32bit_cp0_register(CP0_STATUS, ST0_CU0);
	
	// Let the CPU know we are alive by resetting HAL timer.
	*VR41XX_PMUCNTREG |= 0x4;
	
	// Restore context, necessary if do_wakeup scratch
	// the next reset call should restore wince internal
	// since memory signature is corrupted !
	*WINCE_SIGNATURE_ADD1 = wince_signature_old1;
	*WINCE_SIGNATURE_ADD2 = wince_signature_old2;
	*WINCE_RETURN_ADD = wince_return_old;

#ifdef CONFIG_VR41XX_LED
	if (*VR41XX_LEDCNTREG & 0x0001)
		return;	// Now blinking, so ignore this request
	*VR41XX_LEDHTSREG  = 12;	    // LED on time  (1 = 0.0625 sec)
	*VR41XX_LEDLTSREG  = 3;	        // LED off time (1 = 0.0625 sec)
	*VR41XX_LEDASTCREG = 2;		    // How many times blink
	*VR41XX_LEDCNTREG  = 0x0002;	// Enable auto stop
	*VR41XX_LEDCNTREG |= 0x0001;	// Start blinking
	// Wait for LEDINT
	while ((*VR41XX_LEDINTREG & 0x0001) == 0);
	// Clear LEDINT
	*VR41XX_LEDINTREG = 0x0001;
#endif
	
	// restart kernel
	asm volatile (
		" .set noreorder\n"
		" j kernel_entry\n"
		" nop\n"
		" nop\n"
		" nop\n"
		" .set reorder\n"
		);
}

void vr41xx_extend_hibernate(void)
{
	// wince E15 signature
	wince_signature_old1 = *WINCE_SIGNATURE_ADD1;
	wince_signature_old2 = *WINCE_SIGNATURE_ADD2;
	wince_return_old = *WINCE_RETURN_ADD;

	*WINCE_SIGNATURE_ADD1 = *WINCE_SIGNATURE_CHECK1;
	*WINCE_SIGNATURE_ADD2 = *WINCE_SIGNATURE_CHECK2;
	*WINCE_RETURN_ADD = casio_e15_wakeup; 
	
	printk (KERN_NOTICE "Hibernate signature set, ready for wake up !\n");

}
#endif /* end of CONFIG_PM_SUSPEND_WAKEUP */

void __init nec_vr41xx_setup(void)
{
	set_io_port_base(IO_PORT_BASE);
	ioport_resource.start = IO_PORT_RESOURCE_START;
	ioport_resource.end = IO_PORT_RESOURCE_END;
	iomem_resource.start = IO_MEM_RESOURCE_START;
	iomem_resource.end = IO_MEM_RESOURCE_END;

#ifdef CONFIG_BLK_DEV_INITRD
	ROOT_DEV = MKDEV(RAMDISK_MAJOR, 0);
	initrd_start = (unsigned long)&__rd_start;
	initrd_end = (unsigned long)&__rd_end;
#endif
	
	_machine_restart = vr41xx_restart;
	_machine_halt = vr41xx_halt;
	_machine_power_off = vr41xx_power_off;

	board_time_init = vr4111_time_init;
	board_timer_setup = vr4111_timer_setup;

#ifdef CONFIG_PM
	vr41xx_board_hibernate = vr4111_hibernate;
	vr41xx_board_suspend = vr4111_suspend;
	vr41xx_board_standby = vr4111_wait;
#endif
	
#ifdef CONFIG_BLK_DEV_IDE
	ide_ops = &e15_ide_ops;
#endif

#ifdef CONFIG_FB
	conswitchp = &dummy_con;
#endif

	vr41xx_bcu_init();

	vr41xx_siu_init(0, SIU_RS232C, 0);
	
	// Insure that vr41xx_dma_sem is initialized as unlocked, even
	// in the case of a failed hibernate/wakeup:
	init_MUTEX(&vr41xx_dma_sem);

#ifdef CONFIG_VR41XX_LED
	vr41xx_xblink_led(2, 12, 3);
#endif
}

