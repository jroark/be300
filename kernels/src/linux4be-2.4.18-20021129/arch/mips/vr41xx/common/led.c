/*
 * linux/arch/mips/vr41xx/led.c
 *
 * VR41xx LED control unit (LCU) driver
 *
 * Copyright (C) 1999 Hiroshi Kawashima (kawashima@iname.com)
 * Copyright (C) 2001 François Leblanc <francois.leblanc@cev-sa.com>
 * 
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/kernel.h>
#include <asm/vr41xx.h>

void vr41xx_xblink_led(unsigned int times, unsigned int time_on, unsigned int time_off)
{
	time_on &= 0x007f;	// only lower 7 bit is used
	if (time_on == 0)
		return;	// Illegal value
	
	time_off &= 0x007f;	// only lower 7 bit is used
	if (time_off == 0)
		return;	// Illegal value

	if (*VR41XX_LEDCNTREG & 0x0001)
		return;	// Now blinking, so ignore this request
	*VR41XX_LEDHTSREG  = time_on;	// LED on time  (1 = 0.0625 sec)
	*VR41XX_LEDLTSREG  = time_off;	// LED off time (1 = 0.0625 sec)
	*VR41XX_LEDASTCREG = times;		// How many times blink
	*VR41XX_LEDCNTREG  = 0x0002;	// Enable auto stop
	*VR41XX_LEDCNTREG |= 0x0001;	// Start blinking
	// Wait for LEDINT
	while ((*VR41XX_LEDINTREG & 0x0001) == 0)
		;
	// Clear LEDINT
	*VR41XX_LEDINTREG = 0x0001;
}

void vr41xx_blink_led(unsigned int times, unsigned int length)
{
	/* keep compatible */
	vr41xx_xblink_led(times, length, 1);
}

