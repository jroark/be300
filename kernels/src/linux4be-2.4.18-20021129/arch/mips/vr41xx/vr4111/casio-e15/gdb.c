/*
 * arch/mips/vr41xx/vr4111/casio-e15/gdb.c
 *     provide fonctions for gdb use.
 *
 * Copyright (C) 2002 Francois Leblanc <francois.leblanc@cev-sa.com>
 *
 * This program is free software; you can redistribute	it and/or modify it
 * under  the terms of	the GNU General	 Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */
#include <linux/config.h>
#include <linux/types.h>
#include <linux/serial.h>
#include <linux/serial_reg.h>
#include <asm/vr41xx.h>

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



