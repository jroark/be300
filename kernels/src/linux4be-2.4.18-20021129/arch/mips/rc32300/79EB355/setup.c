/*
 * BRIEF MODULE DESCRIPTION
 *	IDT 79EB355 board setup.
 *
 * Copyright 2002 MontaVista Software Inc.
 * Author: MontaVista Software, Inc.
 *         	stevel@mvista.com or source@mvista.com
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 *  THIS  SOFTWARE  IS PROVIDED   ``AS  IS'' AND   ANY  EXPRESS OR IMPLIED
 *  WARRANTIES,   INCLUDING, BUT NOT  LIMITED  TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
 *  NO  EVENT  SHALL   THE AUTHOR  BE    LIABLE FOR ANY   DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 *  NOT LIMITED   TO, PROCUREMENT OF  SUBSTITUTE GOODS  OR SERVICES; LOSS OF
 *  USE, DATA,  OR PROFITS; OR  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 *  ANY THEORY OF LIABILITY, WHETHER IN  CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 *  THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the  GNU General Public License along
 *  with this program; if not, write  to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/irq.h>
#include <linux/ioport.h>
#include <asm/bootinfo.h>
#include <asm/io.h>
#include <asm/mipsregs.h>
#include <asm/pgtable.h>
#include <asm/reboot.h>
#include <asm/rc32300/rc32300.h>

extern void (*board_time_init)(void);
extern void (*board_timer_setup)(struct irqaction *irq);
extern void rc32300_time_init(void);
extern void rc32300_timer_setup(struct irqaction *irq);
extern char * __init prom_getcmdline(void);

extern void rc32300_restart(char *);
extern void rc32300_halt(void);
extern void rc32300_power_off(void);

extern int init_lcd(void);

#ifdef CONFIG_BLK_DEV_INITRD
extern unsigned long initrd_start, initrd_end;
extern void * __rd_start, * __rd_end;
#endif

#if 0
static void dump_dev(int devnum)
{
	printk("DEV%d_BASE   = %08x\n", devnum,
	       rc32300_readl(DEV0_BASE + devnum*DEV_REG_OFFSET));
	printk("DEV%d_MASK   = %08x\n", devnum,
	       rc32300_readl(DEV0_MASK + devnum*DEV_REG_OFFSET));
	printk("DEV%d_CNTL   = %08x\n", devnum,
	       rc32300_readl(DEV0_CNTL + devnum*DEV_REG_OFFSET));
	printk("DEV%d_TIMING = %08x\n", devnum,
	       rc32300_readl(DEV0_TIMING + devnum*DEV_REG_OFFSET));
}
#endif


void __init idt_setup(void)
{
	char* argptr;
	
	argptr = prom_getcmdline();
#ifdef CONFIG_SERIAL_CONSOLE
	if ((argptr = strstr(argptr, "console=")) == NULL) {
		argptr = prom_getcmdline();
		strcat(argptr, " console=ttyS0,9600");
	}
#endif

	board_time_init = rc32300_time_init;
	board_timer_setup = rc32300_timer_setup;

	_machine_restart = rc32300_restart;
	_machine_halt = rc32300_halt;
	_machine_power_off = rc32300_power_off;

	set_io_port_base(KSEG1);

	// clear out any wired entries
	write_32bit_cp0_register(CP0_WIRED, 0);

	/*
	 * Setup Device 3. The EPLD (U13) splits device 3 chip-select
	 * into seperate chip selects for the TDM, LCD, and RTC
	 * devices.
	 */
	rc32300_writel(0x00000000, DEV0_MASK   + 3*DEV_REG_OFFSET);
	rc32300_writel(TDM_BASE,   DEV0_BASE   + 3*DEV_REG_OFFSET);
	/* timings are from IDT/sim source */
	rc32300_writel(0x0FFFFF84, DEV0_CNTL   + 3*DEV_REG_OFFSET);
	rc32300_writel(0x00001FFF, DEV0_TIMING + 3*DEV_REG_OFFSET);
	rc32300_writel(0xFFFF0000, DEV0_MASK   + 3*DEV_REG_OFFSET);
	
	/* initialize the LCD panel */
	init_lcd();

	idtprintf("IDT 79EB355 Eval MVL 2.1");

#ifdef CONFIG_MTD
	/*
	 * Setup device 2 for flash devices. Set for
	 * 32-bit databus size, write-enable.
	 */
	rc32300_writel(0x00000000, DEV0_MASK   + 2*DEV_REG_OFFSET);
	rc32300_writel(FLASH_BASE, DEV0_BASE   + 2*DEV_REG_OFFSET);
	/* timings are from IDT/sim source */
	rc32300_writel(0x03CF3316, DEV0_CNTL   + 2*DEV_REG_OFFSET);
	rc32300_writel(0x00001133, DEV0_TIMING + 2*DEV_REG_OFFSET);
	rc32300_writel(0xFF800000, DEV0_MASK   + 2*DEV_REG_OFFSET);
#endif
	
#ifdef CONFIG_BLK_DEV_INITRD
	ROOT_DEV = MKDEV(RAMDISK_MAJOR, 0);
	initrd_start = (unsigned long)&__rd_start;
	initrd_end = (unsigned long)&__rd_end;
#endif
}
