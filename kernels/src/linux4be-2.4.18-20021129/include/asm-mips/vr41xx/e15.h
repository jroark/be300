/*
 * FILE NAME
 *	include/asm-mips/vr41xx/cassiopeia/e15.h
 *
 * BRIEF MODULE DESCRIPTION
 *	Include file for CASIO CASSIOPEIA E-15.
 *
 * Copyright 2002 Yoichi Yuasa
 *                yuasa@hh.iij4u.or.jp
 *          
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or (at your
 *  option) any later version.
 */
/*
 * Changes:
 * Francois Leblanc <francois.leblanc@cev-sa.com> Wed, 27 Mar 2002
 * - Report dependant config from vr41xx-platdep.h here.
 * 
 */
#ifndef __CASIO_E15_H
#define __CASIO_E15_H

#include <asm/addrspace.h>
#include <asm/vr41xx.h>

/*
 * Board specific address mapping
 */
#define VR41XX_ISA_MEM_BASE		0x10000000
#define VR41XX_ISA_MEM_SIZE		0x04000000

#define VR41XX_ISA_IO_BASE		0x14000000
#define VR41XX_ISA_IO_SIZE		0x04000000

#define IO_PORT_BASE            KSEG1ADDR(VR41XX_ISA_IO_BASE)
#define IO_PORT_RESOURCE_START  0
#define IO_PORT_RESOURCE_END    VR41XX_ISA_IO_SIZE
#define IO_MEM_RESOURCE_START   VR41XX_ISA_MEM_BASE
#define IO_MEM_RESOURCE_END     (VR41XX_ISA_MEM_BASE + VR41XX_ISA_MEM_SIZE)

/*
 * Board specific hardware description
 */
#define VIDEORAM_BASE     (KSEG1 + 0x0a000000)
#define VIDEORAM_SIZE     (256 * 1024) /* ??? */
#define FB_X_RES          240
#define FB_X_VIRTUAL_RES  512
#define FB_Y_RES          320
#define FB_BPP            4
#define FB_IS_GREY        1
#define FB_IS_INVERSE     1 

// GPIO[27] is speaker power on/off bit
#define VR41XX_ENABLE_SPEAKER()		\
	{				\
		int flags;	\
		save_and_cli(flags);	\
		*VR41XX_GIUPIODH |= VR41XX_GIUPIODH_GPIO27; \
		restore_flags(flags);	\
	}
#define VR41XX_DISABLE_SPEAKER()		\
	{				\
		int flags;	\
		save_and_cli(flags);	\
		*VR41XX_GIUPIODH &= ~VR41XX_GIUPIODH_GPIO27; \
		restore_flags(flags);	\
	}

/*
 * LCD control management 
 */
extern void gpiolcd_setup(void);
extern int gpiolcd_backlight(int n);
extern int get_gpiolcd_backlight(void);
extern int gpiolcd_contrast(int n);
extern int get_gpiolcd_contrast(void);
extern int gpiolcd_lcdpower(int on);
#define	LCD_SETUP() gpiolcd_setup()
#define	LCD_BACKLIGHT(n) gpiolcd_backlight(n) 
#define	GET_LCD_BACKLIGHT() get_gpiolcd_backlight()
#define	LCD_CONTRAST(n) gpiolcd_contrast(n)
#define	GET_LCD_CONTRAST() get_gpiolcd_contrast()
#define	LCD_POWER(n) gpiolcd_lcdpower(n)

/* GPIO buttons mapping from GPIO0 to GPIO31 */
#define GPIO_BTN_MAP { \
	0, BTN_AP13, BTN_AP14, BTN_AP15, BTN_AP16, BTN_POWER_GPIO, BTN_ACTION, BTN_EXIT, \
	BTN_AP3, BTN_AP2, BTN_AP1, BTN_AP4, BTN_DOWN, BTN_UP, BTN_DOWN, BTN_AP12, \
	BTN_AP6, BTN_AP7, BTN_NOTIFICATION, BTN_AP8, BTN_NOTIFICATION, BTN_AP9, BTN_AP10, BTN_AP11, \
	0, 0, 0, 0, 0, BTN_NORTH, BTN_WEST, BTN_EAST \
}

#define GPIO_BTN_PRESS_LOW

#define VR41XX_ENABLE_SERIAL(x)		do { } while (0)
#define VR41XX_DISABLE_SERIAL(x)	do { } while (0)
#define VR41XX_ENABLE_IRDA()		do { } while (0)
#define VR41XX_DISABLE_IRDA()		do { } while (0)

#endif /* __CASIO_E15_H */
