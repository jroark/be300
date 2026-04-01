/* 
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1999 by Michael Klar
 */
/*
 * Changes:
 * Francois Leblanc <francois.leblanc@cev-sa.com> Wed, 27 Mar 2002
 * - Report hardware description in each corresponding header.
 * 
 */
#ifndef __ASM_MIPS_VR41XX_PLATDEP_H 
#define __ASM_MIPS_VR41XX_PLATDEP_H 

/*
 * This file contains the device-specific defines for VR41xx CPU-based
 * platforms.  Anything CPU-specific should go in vr41xx.h instead.
 * Eventually, most, if not all, dependancies on CONFIG_[device_name]
 * should be moved in here, which should make it easier to add new
 * device support.
 */

#include <linux/config.h>
#include <asm/vr41xx.h>

/*
 * Here are the macros defined in this file and what they mean.  If not
 * defined for a platform, reasonable defaults are used.
 *
 * DEVICE_IRQ_MASKL	IRQ mask for GPIO ints, to disable some IRQs for
 *			autodetect.  A bitmask corresponding to IRQ #s
 *			40 through 55.  A 0 bit means disabled, a 1 bit
 *			will enable for autodetect only if the line is
 *			configured as input at boot.
 * DEVICE_IRQ_MASKH	Same for IRQ #s 56 through 71.  Note that a VR4111
 * 			or VR4121-based device that uses 32-bit data bus
 *			should probably set DEVICE_IRQ_MASKH to 0.
 * ADJUSTED_PORT_BASE	Some devices have ISA IO ports at a different
 *			offset than the standard.  In particular,
 *			VRC4171 PCMCIA controller needs an extra offset.
 *			This value is the virtual address corresponding
 *			to ISA IO port 0.
 * For VIDEORAM_* ad FB_*, see drivers/video/sfb.c for description, those
 * are not used unless Simple Frame Buffer (or maybe one of its
 * derivatives) is used.
 * GPIO_BTN_MAP		Default map of GPIO lines to button definition.
 * GPIO_BTN_PRESS_LOW	Define this if GPIO level 0 corresponds to a
 *			button press.  Leave undefined if 1 correspods
 *			to button press.
 * KBD_SCANLINES	For devices with keyboards, the number of KIU
 *			scanlines to use.  Default is all 12.
 *
 * (more to come...)
 */

#ifdef CONFIG_CASIO_E15
#include <asm/vr41xx/e15.h>
#endif

#ifdef CONFIG_CASIO_E55
#include <asm/vr41xx/e55.h>
#endif


// Some reasonable defaults

#ifndef DEVICE_IRQ_MASKL
#define DEVICE_IRQ_MASKL 0xffff
#endif

#ifndef DEVICE_IRQ_MASKH
#ifndef CONFIG_CPU_VR4181
#define DEVICE_IRQ_MASKH 0xffff
#endif
#endif

#ifndef ADJUSTED_PORT_BASE
#define ADJUSTED_PORT_BASE VR41XX_PORT_BASE
#endif

#ifndef VR41XX_ENABLE_SPEAKER
#define	VR41XX_ENABLE_SPEAKER()		do { } while (0)
#endif
#ifndef VR41XX_DISABLE_SPEAKER
#define	VR41XX_DISABLE_SPEAKER()	do { } while (0)
#endif

#ifndef VR41XX_ENABLE_SERIAL
#define VR41XX_ENABLE_SERIAL(x)		do { } while (0)
#endif
#ifndef VR41XX_DISABLE_SERIAL
#define VR41XX_DISABLE_SERIAL(x)	do { } while (0)
#endif

#ifndef VR41XX_ENABLE_IRDA
#define VR41XX_ENABLE_IRDA()		do { } while (0)
#endif
#ifndef VR41XX_DISABLE_IRDA
#define VR41XX_DISABLE_IRDA()		do { } while (0)
#endif

#ifndef KBD_SCANLINES
#define KBD_SCANLINES 12
#endif

#endif /* __ASM_MIPS_VR41XX_PLATDEP_H */
