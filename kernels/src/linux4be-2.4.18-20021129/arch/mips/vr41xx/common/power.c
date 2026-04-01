/* 
 * VR41xx reset and power management interface
 * PM User interface loosely based on Andrew Henroid's i386 ACPI driver
 *
 * Copyright (C) 2000 Michael Klar
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 */
/*
 * Changes:
 * Francois Leblanc <francois.leblanc@cev-sa.com> Wed, 27 Mar 2002
 * - Add extend hibernate function to let board manage specific 
 *   actions before hibernate like casio E15 witch update win ce
 *   memory signature before halt and so reboot in linux.
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/config.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/sysctl.h>
#include <linux/pm.h>
#include <linux/acpi.h>
#include <linux/irq.h>	/* for disable_irq, enable_irq, to be removed */
#include <linux/reboot.h>
#include <asm/vr41xx.h>
#include <asm/cacheops.h>
#include <asm/mipsregs.h>
#include <asm/pgalloc.h>
#include <asm/power.h>

/*
 * vr41xx_dma_sem is needed because we cannot suspend while a DMA transfer
 * is in progress, otherwise the CPU will be left in an undefined state.
 */
extern struct semaphore vr41xx_dma_sem;

extern void vr41xx_extend_hibernate(void);

void (*vr41xx_board_hibernate)(void);
void (*vr41xx_board_suspend)(void);
void (*vr41xx_board_standby)(void);

#ifdef CONFIG_PM_SUSPEND_WAKEUP

//
// On VR41xx CPUs, the hibernate instruction will cut power to most of
// the device.  On a wakeup event (usually power button), the CPU
// restarts at the reset vector in ROM, so the bootloader has to run
// again in order to pass control back to the kernel.  As a result,
// the bootloader (and/or some level of code in the ROM) must not
// disturb the contents of RAM before passing control to the kernel
// when waking from hibernate.
//
// This code uses the power management kernel interface to the
// peripheral drivers.  It is not real ACPI support, which would require
// some rather impractical (for this class of devices) hardware and
// firmware extensions.  Also, for now, the SUSPEND and RESUME functions
// in the peripheral drivers just need to do any necessary hardware
// state saving and restoring.  Power, IRQ mask, clock mask, and GPIO
// state are handled globally.  This will probably change in the future
// to allow for power-state control of individual peripheral units.
//
// Right now, the data arg passed to the pm_request callback for SUSPEND
// and RESUME means:
//   0: restore state and return to powered-on
//   1: save state and prepare for hibernate, no need to power off because
//      power will be removed globally
//   2: save state and prepare for suspend, global power will remain, so
//      the peripheral driver may want to power down its local power
//   3: prepare for suspend, peripherals should appear to user to be
//      running, most interrupts can wake from this state
// The numbers above should get replaced with an enum evetually, and
// the requirements for each mode will probably change, since the above
// doesn't really make sense (eg, no need to save state for 2, except
// that we need something to restore to for 0).  The pm_request callbacks
// currently ignore the data value anyway....
//
// This may seem like a bad idea to put the state information into the
// data segment where the bootloader may overwrite it, but if the
// bootloader rewrites data (and/or bss), it won't support wake from
// hibernate, anyway.
//
unsigned int powerevent_queued;
unsigned int hibernation_state = LOAD_MAGIC;
static unsigned short clkmsk_state;
static unsigned short more_gpioregs_to_save[2];

#ifndef VR41XX_IRQ_MAX      
#define VR41XX_IRQ_MAX ((VR41XX_NUM_CPU_IRQ)+(VR41XX_NUM_SYS_IRQ)+(VR41XX_NUM_GPIO_IRQ))
#endif

//
//
// Unlike the real pm_request callbacks, this one doesn't get registered
// with PM, and only gets called from do_hibernate and do_wakeup, because
// it has to happen in a certain order.  It also assumes ints disabled.
//
static void do_pm_irq_request(pm_request_t rqst)
{
	static unsigned short irq_mask[5];
	unsigned int status;

	switch (rqst) {
	case PM_RESUME:
		status = read_32bit_cp0_register(CP0_STATUS) & 0xffff00ff;
		write_32bit_cp0_register(CP0_STATUS, status | irq_mask[0]);
		*VR41XX_MSYSINT1REG = irq_mask[1];
		*VR41XX_MSYSINT2REG = irq_mask[2];
#ifdef CONFIG_CPU_VR4181
		*VR41XX_GPINTMSK = irq_mask[3];
#else
		*VR41XX_MGIUINTLREG = irq_mask[3];
		*VR41XX_MGIUINTHREG = irq_mask[4];
#endif
		break;
	case PM_SUSPEND:
		irq_mask[0] = read_32bit_cp0_register(CP0_STATUS) & 0xff00;
		irq_mask[1] = *VR41XX_MSYSINT1REG;
		irq_mask[2] = *VR41XX_MSYSINT2REG;
#ifdef CONFIG_CPU_VR4181
		irq_mask[3] = *VR41XX_GPINTMSK;
#else
		irq_mask[3] = *VR41XX_MGIUINTLREG;
		irq_mask[4] = *VR41XX_MGIUINTHREG;
#endif
		break;
	}
}

#ifdef CONFIG_CPU_VR4181
#define MIN_GPIOREG VR41XX_GPMD0REG
#define MAX_GPIOREG VR41XX_LCDGPMODE
#else
#define MIN_GPIOREG VR41XX_GIUIOSELL
#define MAX_GPIOREG VR41XX_GIUPODATH
#endif
#define NR_GPIOREGS (((long)MAX_GPIOREG - (long)MIN_GPIOREG) / sizeof(short) + 1)

unsigned short gpio_state[NR_GPIOREGS];

asmlinkage void do_hibernate(void *sp)
{
	int i;

	cli();
	powerevent_queued = 0;

	if (pm_send_all(PM_SUSPEND, (void *)1)) {
		sti();
		up(&vr41xx_dma_sem);
		return;
	}
	do_pm_irq_request(PM_SUSPEND);
	for (i = 0; i < NR_GPIOREGS; i++)
		gpio_state[i] = *(MIN_GPIOREG + i);
	more_gpioregs_to_save[0] = *VR41XX_GIUUSEUPDN;
	more_gpioregs_to_save[1] = *VR41XX_GIUTERMUPDN;
	clkmsk_state = *VR41XX_CMUCLKMSK;
	
	// do more board specific hibernate code
#ifdef CONFIG_VR41XX_EXTEND_HIBERNATE
	vr41xx_extend_hibernate();
#endif
	
	// we may want to put in some checksum or CRC on all of RAM, too
	hibernation_state = HIB_MAGIC;
	flush_cache_all();
	vr41xx_board_hibernate();
	// never reached...
}

extern void DbgInitSerial(void);

asmlinkage int do_wakeup(void)
{
	int i, retval;

	// start out with something reasonable in CP0_STATUS, this will get
	// replaced when context is restored.  ints disbaled at this point
	write_32bit_cp0_register(CP0_STATUS, ST0_CU0);

	// Do what loadmmu would have done if cold init
	change_cp0_config(CONF_CM_CMASK, CONF_CM_CACHABLE_NONCOHERENT);
	flush_cache_all();
	write_32bit_cp0_register(CP0_WIRED, 0);
	set_pagemask(PM_4K);
	flush_tlb_all();

	// this is about the point where we would check CRC if we did one

	*VR41XX_CMUCLKMSK = clkmsk_state;
	*VR41XX_GIUUSEUPDN = more_gpioregs_to_save[0];
	*VR41XX_GIUTERMUPDN = more_gpioregs_to_save[1];
	for (i = 0; i < NR_GPIOREGS; i++)
		*(MIN_GPIOREG + i) = gpio_state[i];
	do_pm_irq_request(PM_RESUME);
	retval = pm_send_all(PM_RESUME, (void *)0);
	
	if (!retval) {
		sti();
		up(&vr41xx_dma_sem);
	}
	return retval;
}

/*
 * Enter system sleep state (hibernate, really soft-off)
 *
 * Actually, all this does is to set a flag, so the tail end of the syscall
 * handler jumps to do_hibernate above, rather than executing
 * o32_ret_from_sys_call.  That just makes for a convenient place to restore
 * context from, since context is already saved on the stack at that point,
 * and bottom-half processing already done.  The wakeup part is done in
 * kernel_entry, since waking up from hibernate will restart at reset vector.
 */
static int pm_do_sleep(ctl_table *ctl,
			 int write,
			 struct file *file,
			 void *buffer,
			 size_t *len)
{
	if (!write) {
		if (file->f_pos) {
			*len = 0;
			return 0;
		}
	} else {
		if (file->f_flags & O_NONBLOCK) {
                	if (down_trylock(&vr41xx_dma_sem))
                	        return -EAGAIN;
	        } else {
	                if (down_interruptible(&vr41xx_dma_sem))
        	                return -ERESTARTSYS;
	        }

		powerevent_queued = 1;
	}
	file->f_pos += *len;
	return 0;
}

#endif // CONFIG_PM_SUSPEND_WAKEUP

#ifdef CONFIG_PM_POWERED_SUSPEND

/*
 * Enter system "suspend" state, CPU remains powered but most/all clocks off
 */
static int pm_do_suspend(ctl_table *ctl,
			 int write,
			 struct file *file,
			 void *buffer,
			 size_t *len)
{
	if (!write) {
		if (file->f_pos) {
			*len = 0;
			return 0;
		}
	} else {
		int retval;

		// Must not suspend while DMA in progress
		if (file->f_flags & O_NONBLOCK) {
                	if (down_trylock(&vr41xx_dma_sem))
                	        return -EAGAIN;
	        } else {
	                if (down_interruptible(&vr41xx_dma_sem))
        	                return -ERESTARTSYS;
	        }

		retval = pm_send_all(PM_SUSPEND, (void *)2);
		if (retval) {
			up(&vr41xx_dma_sem);
			return retval;
		}
		vr41xx_board_suspend();
		
		retval = pm_send_all(PM_RESUME, (void *)0);
		up(&vr41xx_dma_sem);
		if (retval)
			return retval;
	}
	file->f_pos += *len;
	return 0;
}

#endif // CONFIG_PM_POWERED_SUSPEND

#ifdef CONFIG_PM_STANDBY

/*
 * Enter system "standby" state, CPU remains powered, clocks on
 */
static int pm_do_standby(ctl_table *ctl,
			 int write,
			 struct file *file,
			 void *buffer,
			 size_t *len)
{
	if (!write) {
		if (file->f_pos) {
			*len = 0;
			return 0;
		}
	} else {
// MFK: replace with send PM_SUSPEND request to pm_time_request:
		disable_irq(VR41XX_IRQ_INT1);
		vr41xx_board_standby ();
// MFK: replace with send PM_RESUME request to pm_time_request:
		enable_irq(VR41XX_IRQ_INT1);
	}
	file->f_pos += *len;
	return 0;
}

#endif // CONFIG_PM_STANDBY

/*
 * NOTE1: We're hijacking the ACPI binary ctl_name's for now.  Don't expect
 *        this to necessarily behave as ACPI does, some are blatently wrong.
 * NOTE2: Don't get used to the text names, either, they are subject to change
 *        if we ever come up with a real naming convention....
 * NOTE3: For that matter, don't get used to this being done via sysctl,
 *        that's subject to change, too.
 */
static struct ctl_table pm_table[] =
{
#ifdef CONFIG_PM_SUSPEND_WAKEUP
	{ACPI_SLEEP, "sleep", NULL, 0, 0600, NULL, &pm_do_sleep},
#endif
#ifdef CONFIG_PM_POWERED_SUSPEND
	{ACPI_S1_SLP_TYP, "suspend", NULL, 0, 0600, NULL, &pm_do_suspend},
#endif
#ifdef CONFIG_PM_STANDBY
	{ACPI_S0_SLP_TYP, "standby", NULL, 0, 0600, NULL, &pm_do_standby},
#endif
	{0}
};

static struct ctl_table pm_dir_table[] =
{
	{CTL_ACPI, "pm", NULL, 0, 0555, pm_table},
	{0}
};

/*
 * Initialize the power management user interface
 */
static int __init pm_init(void)
{
	register_sysctl_table(pm_dir_table, 1);
	return 0;
}

__initcall(pm_init);

