/*
 * include/asm-mips/vr41xx.h
 *
 * Primary header for NEC VR41xx processors.
 *
 * Copyright (C) 1999 Michael Klar
 * Copyright (C) 2001, 2002 Paul Mundt
 * Copyright (C) 2002 MontaVista Software, Inc.
 * Copyright (C) 2002 TimeSys Corp.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */
#ifndef __ASM_MIPS_VR41XX_H
#define __ASM_MIPS_VR41XX_H

#include <linux/config.h>
#include <linux/interrupt.h>

/*
 * Any code being written for a VR41xx device should be including this
 * header.. especially if it's a driver. Directly including a header for a
 * specific member of the family should be avoided, as should any
 * CONFIG_VR41XX checks in drivers when its something as simple as the
 * location of a certain register not being consistent across all members of
 * the family (such as in the case of the DSU).
 *
 * The headers that are included by this header are to have the naming
 * convention of VR41XX_MACRO at all times, and should _never_ under _any_
 * circumstances be referencing VR4181_MACRO (in the case of the vr4181). Any
 * member specific macros need to go somewhere else.
 */
#if defined(CONFIG_VR4111)
  #include <asm/vr4111/vr4111.h>
#elif defined(CONFIG_VR4121)
  #include <asm/vr4121/vr4121.h>
#elif defined(CONFIG_VR4122)
  #include <asm/vr4122/vr4122.h>
#elif defined(CONFIG_VR4181)
  #include <asm/vr4181/vr4181.h>
#endif

/*
 * Bus Control Uint
 */
extern void vr41xx_bcu_init(void);

/*
 * Clock Mask Unit
 */
extern void vr41xx_clock_supply(u16 mask);
extern void vr41xx_clock_mask(u16 mask);

/*
 * Interrupt Control Unit
 */

/* GIU Interrupt Numbers */
#define GIUL_IRQ(x)	(40 + (x))
#define GIUH_IRQ(x)	(56 + (x))

struct irqcascade {
	int cascade;
	int (*get_irq_number)(int irq);
};

extern void (*board_irq_init)(void);
extern void vr41xx_board_irq_init(void);
extern void vr41xx_cascade_irq(unsigned int irq, int (*get_irq_number)(int irq));

/*
 * Serial Interface Unit
 */

/* casiopeia be300 doesn't use siu and dsiu on the vr4131 */
#ifndef CONFIG_CASIO_BE300

extern void vr41xx_siu_init(int line, int interface, int module);
extern void vr41xx_siu_ifselect(int interface, int module);

/* SIU interfaces */
enum {
	SIU_RS232C,
	SIU_IRDA
};

/* IrDA interfaces */
enum {
	IRDA_SHARP = 1,
	IRDA_TEMIC,
	IRDA_HP
};

/*
 * Debug Serial Interface Unit
 */
extern void vr41xx_dsiu_init(int line);
#endif /* CONFIG_CASIO_BE300 */

/*
 * PCI Control Unit
 */
extern void vr41xx_pciu_init(void);

/*
 * MISC
 */
extern void vr41xx_time_init(void);
extern void vr41xx_timer_setup(struct irqaction *irq);

extern void vr41xx_restart(char *command);
extern void vr41xx_halt(void);
extern void vr41xx_power_off(void);

#endif /* __ASM_MIPS_VR41XX_H */

