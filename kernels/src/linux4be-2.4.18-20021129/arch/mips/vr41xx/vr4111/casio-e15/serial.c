/*
 * linux/drivers/char/serial.c
 * Serial (SIU) driver for NEC VR41xx CPUs, VR4102 and up only.
 *
 * Based almost entirely on linux/drivers/char/serial.c
 * Modified for VR41xx by Michael Klar, mfklar@ponymail.com
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 */
/*
 * Changes:
 * François Leblanc <francois.leblanc@cev-sa.com> Mon, 13 Apr 2002
 * - Append missing function and minor syntax changes.
 */

#include <linux/config.h>
#include <linux/pm.h>
#include <linux/init.h>
#include <linux/serial.h>
#include <linux/serial_reg.h>
#include <asm/serial.h>
#include <asm/vr41xx.h>
#include <asm/vr41xx-platdep.h>

#define DEVICE_NAME "Serial pm manager"

#ifndef MAX_VR_PORT
#define MAX_VR_PORT 1
#endif

static unsigned int serial_powered_on;

#ifdef CONFIG_PM
static unsigned char fcr_shadow[MAX_VR_PORT+1];
#endif

#ifdef CONFIG_PM
static int pm_serial_request(struct pm_dev *dev, pm_request_t rqst, void *data)
{
#ifndef CONFIG_CPU_VR4181
	static unsigned char save_state[6];
#else
	static unsigned char save_state[12];
#endif
	unsigned char dummy;
	int flags;

	/* if not powered on, do nothing 
	if (!serial_powered_on)
		return 0;
	*/

	switch (rqst) {
	case PM_SUSPEND:
		/* wait for anything in the xmit FIFO to send */
		save_and_cli(flags);
		while (!(*VR41XX_SIULS & UART_LSR_TEMT))
			barrier();
		save_state[0] = *VR41XX_SIULC;
		*VR41XX_SIULC = UART_LCR_DLAB;	/* DLAB = 1 to read baud rate */
		barrier();
		save_state[1] = *VR41XX_SIUDLL;
		save_state[2] = *VR41XX_SIUDLM;
		barrier();
		*VR41XX_SIULC = 0;
		barrier();
		save_state[3] = *VR41XX_SIUIE;
		*VR41XX_SIUIE = save_state[3] & ~UART_IER_THRI;
// MFK: should also power down, cut clocks, and disable receive int, too, if
// wakeup not enabled for this device.
// MFK: should probably also enable DCD IRQ if wakeup is enabled
		save_state[4] = *VR41XX_SIUMC;
#ifndef CONFIG_CPU_VR4181
		save_state[5] = *VR41XX_SIUIRSEL;
#else /* VR4181 has a 2nd UART */
		while (!(*VR41XX_SIULS_2 & UART_LSR_TEMT))
			barrier();
		save_state[5] = *VR41XX_SIULC_2;
		*VR41XX_SIULC_2 = UART_LCR_DLAB;	/* DLAB = 1 to read baud rate */
		barrier();
		save_state[6] = *VR41XX_SIUDLL_2;
		save_state[7] = *VR41XX_SIUDLM_2;
		barrier();
		*VR41XX_SIULC_2 = 0;
		barrier();
		save_state[8] = *VR41XX_SIUIE_2;
		*VR41XX_SIUIE_2 = save_state[8] & ~UART_IER_THRI;
		save_state[9] = *VR41XX_SIUMC_2;
		save_state[10] = *VR41XX_SIUIRSEL_2;
		save_state[11] = *VR41XX_SIUCSEL_2;
#endif
		restore_flags(flags);
		break;
	case PM_RESUME:
// MFK: it's too late to disable ints here, if it was serial data that woke us
// up.  Need to check in the interrupt handler if we're still in suspend, then
// throw out the data there if we are (because data in FIFO will be bogus).
		save_and_cli(flags);
		/* clear out any pending data or interrupts */
		*VR41XX_SIUFC = UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT;
		dummy = *VR41XX_SIURB;
		dummy = *VR41XX_SIUMS;
		dummy = *VR41XX_SIUIID;
		barrier();
		while ( *VR41XX_SIULS & UART_LSR_DR )
			dummy = *VR41XX_SIURB;

		*VR41XX_SIULC = UART_LCR_DLAB;
		barrier();
		*VR41XX_SIUDLL = save_state[1];
		*VR41XX_SIUDLM = save_state[2];
		barrier();
		*VR41XX_SIULC = 0;
		barrier();
		*VR41XX_SIUIE = save_state[3];
		*VR41XX_SIUMC = save_state[4];
#ifndef CONFIG_CPU_VR4181
		*VR41XX_SIUIRSEL = save_state[5];
#endif
		barrier();
		*VR41XX_SIULC = save_state[0];
#ifndef CONFIG_CPU_VR4181
		*VR41XX_SIUFC = fcr_shadow[(save_state[5] & 1) ? 1 : 0 ];
#else
		*VR41XX_SIUFC = fcr_shadow[0];

		/* same as above for 2nd UART on VR4181 */
		*VR41XX_SIUFC_2 = UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT;
		dummy = *VR41XX_SIURB_2;
		dummy = *VR41XX_SIUMS_2;
		dummy = *VR41XX_SIUIID_2;
		barrier();
		while ( *VR41XX_SIULS_2 & UART_LSR_DR )
			dummy = *VR41XX_SIURB_2;

		*VR41XX_SIULC_2 = UART_LCR_DLAB;
		barrier();
		*VR41XX_SIUDLL_2 = save_state[6];
		*VR41XX_SIUDLM_2 = save_state[7];
		barrier();
		*VR41XX_SIULC_2 = 0;
		barrier();
		*VR41XX_SIUIE_2 = save_state[8];
		*VR41XX_SIUMC_2 = save_state[9];
		*VR41XX_SIUIRSEL_2 = save_state[10];
		*VR41XX_SIUCSEL_2 = save_state[11];
		barrier();
		*VR41XX_SIULC_2 = save_state[5];
		*VR41XX_SIUFC_2 = fcr_shadow[(save_state[10] & 1) ? 1 : 2 ];
#endif
		restore_flags(flags);
		break;
	}
	return 0;
}

static int __init casio_e15_serial_init(void)
{
#if 0
	printk(KERN_DEBUG DEVICE_NAME ": pm_init\n");
	pm_register(PM_ISA_DEV, PM_SYS_COM, pm_serial_request);
	//pm_register(PM_SYS_DEV, PM_SYS_UNKNOWN, pm_serial_request);
#else
	printk(KERN_DEBUG DEVICE_NAME ": pm_init not done\n");
#endif
	return 0;
}

__initcall(casio_e15_serial_init);
#endif

#ifdef CONFIG_REMOTE_DEBUG

/*
 * This is the interface to the remote debugger stub.
 * I've put that here to be able to control the serial
 * device more directly.
 *
 * We're a little paranoid with the barrier()s, just in
 * case the compiler tries to be too cute.
 */

static int initialized = 0;

void DbgInitSerial(void)
{
	unsigned char dummy;

	/* Ensure that serial is set to RS-232C (not IrDA) */
	*VR41XX_SIUIRSEL &= ~VR41XX_SIUIRSEL_SIRSEL;

	/* Supply clocks to all serial units */
	vr41xx_clock_supply(VR41XX_CMUCLKMSK_MSKSIU);
	vr41xx_clock_supply(VR41XX_CMUCLKMSK_MSKDSIU);
	vr41xx_clock_supply(VR41XX_CMUCLKMSK_MSKSSIU);

#if 0
		/* turn on the clocks to the serial port */
	serial_power_on(0);
#endif
	
	*VR41XX_SIULC = UART_LCR_DLAB;	/* prepare to set baud rate */
	barrier();

	*VR41XX_SIUDLL = 10;		/* set 120 here for 9600 */
	*VR41XX_SIUDLM = 0;			/* hardcoded: set to 115200 */
	barrier();

	*VR41XX_SIULC = UART_LCR_WLEN8;	/* clear DLAB, set up for 8N1 */
	barrier();

	*VR41XX_SIUIE = 0;			/* disable interrupts */

	*VR41XX_SIUFC = UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT;
#ifdef CONFIG_PM
	//fcr_shadow[0] = 0;
#endif
    *VR41XX_SIUMC = UART_MCR_RTS | UART_MCR_DTR;  /* set RTS and DTR */

	dummy = *VR41XX_SIURB;		/* clear any pending ints */
	dummy = *VR41XX_SIUMS;
	dummy = *VR41XX_SIUIID;
	barrier();

	 	/* clear the receive buffer (and finish clearing ints) */
	while ( *VR41XX_SIULS & UART_LSR_DR )
		dummy = *VR41XX_SIURB;
}

int putDebugChar(unsigned char c)
{
	if (!initialized) { 		/* need to init device first */
		DbgInitSerial();
		initialized = 1;
	}

	while ( !(*VR41XX_SIULS & UART_LSR_THRE) ) ;
	barrier();

	*VR41XX_SIUTH = c;

	return 1;
}


char getDebugChar(void)
{
	if (!initialized) { 		/* need to init device first */
		DbgInitSerial();
		initialized = 1;
	}

	while ( !(*VR41XX_SIULS & UART_LSR_DR) ) ;
	barrier();

	return(*VR41XX_SIURB);
}

#endif /* CONFIG_REMOTE_DEBUG */

/*
  Local variables:
  compile-command: "gcc -D__KERNEL__ -I../../include -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -fno-strict-aliasing -D__SMP__ -pipe -fno-strength-reduce  -march=i686 -DMODULE -DMODVERSIONS -include ../../include/linux/modversions.h   -DEXPORT_SYMTAB -c serial.c"
  End:
*/
