/*
 * FILE NAME
 *	arch/mips/vr41xx/vr4181/common/icu.c
 *
 * BRIEF MODULE DESCRIPTION
 *	Interrupt Control Unit routines for the NEC VR4181.
 *
 * Author: Yoichi Yuasa
 *         yyuasa@mvista.com or source@mvista.com
 *
 * Copyright 2001,2002 MontaVista Software Inc.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or (at your
 *  option) any later version.
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 *  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 *  TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 *  USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 */
/*
 * Changes:
 *  Paul Mundt <lethal@chaoticdreams.org>
 *  - Modified this code for VR4181 support.
 *  - kgdb support.
 *
 *  MontaVista Software Inc. <yyuasa@mvista.com> or <source@mvista.com>
 *  - New creation, NEC VR4122 and VR4131 are supported.
 *
 *  Yoichi Yuasa <yuasa@hh.iij4u.or.jp>  Tue,  5 Mar 2002
 *  - This file was copied from arch/mips/vr41xx/vr4122/common/icu.c.
 *    Only FILE NAME and BRIEF MODULE DESCRIPTION modified this file.
 */
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/types.h>

#include <asm/io.h>
#include <asm/mipsregs.h>
#include <asm/vr41xx.h>
#include <asm/gdb-stub.h>

#include "icu.h"

extern asmlinkage void vr41xx_handle_interrupt(void);
extern void __init init_generic_irq(void);
extern void mips_cpu_irq_init(u32 irq_base);
extern unsigned int do_IRQ(int irq, struct pt_regs *regs);

/*=======================================================================*/

static void enable_sysint1_irq(unsigned int irq)
{
	u16 val;

	val = readw(MSYSINT1REG);
	val |= (u16)1 << (irq - SYSINT1_IRQ_BASE);
	writew(val, MSYSINT1REG);
}

static void disable_sysint1_irq(unsigned int irq)
{
	u16 val;

	val = readw(MSYSINT1REG);
	val &= ~((u16)1 << (irq - SYSINT1_IRQ_BASE));
	writew(val, MSYSINT1REG);
}

static unsigned int startup_sysint1_irq(unsigned int irq)
{ 
	enable_sysint1_irq(irq);
	return 0; /* never anything pending */
}

#define shutdown_sysint1_irq	disable_sysint1_irq
#define ack_sysint1_irq		disable_sysint1_irq

static void end_sysint1_irq(unsigned int irq)
{
	if (!(irq_desc[irq].status & (IRQ_DISABLED | IRQ_INPROGRESS)))
		enable_sysint1_irq(irq);
}

static struct hw_interrupt_type sysint1_irq_type = {
	"SYSINT1",
	startup_sysint1_irq,
	shutdown_sysint1_irq,
	enable_sysint1_irq,
	disable_sysint1_irq,
	ack_sysint1_irq,
	end_sysint1_irq,
	NULL
};

/*=======================================================================*/

static void enable_sysint2_irq(unsigned int irq)
{
	u16 val;

	val = readw(MSYSINT2REG);
	val |= (u16)1 << (irq - SYSINT2_IRQ_BASE);
	writew(val, MSYSINT2REG);
}

static void disable_sysint2_irq(unsigned int irq)
{
	u16 val;

	val = readw(MSYSINT2REG);
	val &= ~((u16)1 << (irq - SYSINT2_IRQ_BASE));
	writew(val, MSYSINT2REG);
}

static unsigned int startup_sysint2_irq(unsigned int irq)
{ 
	enable_sysint2_irq(irq);
	return 0; /* never anything pending */
}

#define shutdown_sysint2_irq	disable_sysint2_irq
#define ack_sysint2_irq		disable_sysint2_irq

static void end_sysint2_irq(unsigned int irq)
{
	if (!(irq_desc[irq].status & (IRQ_DISABLED | IRQ_INPROGRESS)))
		enable_sysint2_irq(irq);
}

static struct hw_interrupt_type sysint2_irq_type = {
	"SYSINT2",
	startup_sysint2_irq,
	shutdown_sysint2_irq,
	enable_sysint2_irq,
	disable_sysint2_irq,
	ack_sysint2_irq,
	end_sysint2_irq,
	NULL
};

/*=======================================================================*/

static void enable_giuintl_irq(unsigned int irq)
{
	u16 val, mask;

	mask = (u16)1 << (irq - GIUINTL_IRQ_BASE);
	writew(mask, GIUINTSTATL);

	val = readw(MGIUINTLREG);
	val |= mask;
	writew(val, MGIUINTLREG);

	val = readw(GIUINTENL);
	val |= mask;
	writew(val, GIUINTENL);
}

static void disable_giuintl_irq(unsigned int irq)
{
	u16 val, mask;

	mask = (u16)1 << (irq - GIUINTL_IRQ_BASE);
	val = readw(GIUINTENL);
	val &= ~mask;
	writew(val, GIUINTENL);

	val = readw(MGIUINTLREG);
	val &= ~mask;
	writew(val, MGIUINTLREG);

	writew(mask, GIUINTSTATL);
}

static unsigned int startup_giuintl_irq(unsigned int irq)
{ 
	enable_giuintl_irq(irq);
	return 0; /* never anything pending */
}

#define shutdown_giuintl_irq	disable_giuintl_irq
#define ack_giuintl_irq		disable_giuintl_irq

static void end_giuintl_irq(unsigned int irq)
{
	if (!(irq_desc[irq].status & (IRQ_DISABLED | IRQ_INPROGRESS)))
		enable_giuintl_irq(irq);
}

static struct hw_interrupt_type giuintl_irq_type = {
	"GIUINTL",
	startup_giuintl_irq,
	shutdown_giuintl_irq,
	enable_giuintl_irq,
	disable_giuintl_irq,
	ack_giuintl_irq,
	end_giuintl_irq,
	NULL
};

/*=======================================================================*/

static void enable_giuinth_irq(unsigned int irq)
{
	unsigned short val, mask;

	mask = (u16)1 << (irq - GIUINTH_IRQ_BASE);
	writew(mask, GIUINTSTATH);

	val = readw(MGIUINTHREG);
	val |= mask;
	writew(val, MGIUINTHREG);

	val = readw(GIUINTENH);
	val |= mask;
	writew(val, GIUINTENH);
}

static void disable_giuinth_irq(unsigned int irq)
{
	unsigned short val, mask;

	mask = (u16)1 << (irq - GIUINTH_IRQ_BASE);
	val= readw(GIUINTENH);
	val &= ~mask;
	writew(val, GIUINTENH);

	val = readw(MGIUINTHREG);
	val &= ~mask;
	writew(val, MGIUINTHREG);

	writew(mask, GIUINTSTATH);
}

static unsigned int startup_giuinth_irq(unsigned int irq)
{ 
	enable_giuinth_irq(irq);
	return 0; /* never anything pending */
}

#define shutdown_giuinth_irq	disable_giuinth_irq
#define ack_giuinth_irq		disable_giuinth_irq

static void end_giuinth_irq(unsigned int irq)
{
	if (!(irq_desc[irq].status & (IRQ_DISABLED | IRQ_INPROGRESS)))
		enable_giuinth_irq(irq);
}

static struct hw_interrupt_type giuinth_irq_type = {
	"GIUINTH",
	startup_giuinth_irq,
	shutdown_giuinth_irq,
	enable_giuinth_irq,
	disable_giuinth_irq,
	ack_giuinth_irq,
	end_giuinth_irq,
	NULL
};

/*=======================================================================*/

static struct irqcascade vr41xx_irqcascade[32];

static struct irqaction cascade = {no_action, 0, 0, "cascade", NULL, NULL};
static int no_cascade_get_irq_number(int irq) { return -1; }

void vr41xx_cascade_irq(unsigned int irq, int (*get_irq_number)(int irq))
{
	if (GIUINTL_IRQ_BASE <= irq < GIUINTH_IRQ_LAST) {
		vr41xx_irqcascade[irq - GIUINTL_IRQ_BASE].cascade = 1;
		vr41xx_irqcascade[irq - GIUINTL_IRQ_BASE].get_irq_number = get_irq_number;
		setup_irq(irq, &cascade);
	}
}


static void __init vr41xx_icu_irq_init(void)
{
	int i;

	writew(0, MSYSINT1REG);
	writew(0, MSYSINT2REG);
	writew(0, MGIUINTLREG);
	writew(0, MGIUINTHREG);

	writew(0, GIUINTENL);
	writew(0, GIUINTENH);
	writew(0xffff, GIUINTSTATL);
	writew(0xffff, GIUINTSTATH);

	for (i = SYSINT1_IRQ_BASE; i <= GIUINTH_IRQ_LAST; i++) {
		if (i >= SYSINT1_IRQ_BASE && i <= SYSINT1_IRQ_LAST)
			irq_desc[i].handler = &sysint1_irq_type;
		else if (i >= SYSINT2_IRQ_BASE && i <= SYSINT2_IRQ_LAST)
			irq_desc[i].handler = &sysint2_irq_type;
		else if (i >= GIUINTL_IRQ_BASE && i <= GIUINTL_IRQ_LAST)
			irq_desc[i].handler = &giuintl_irq_type;
		else if (i >= GIUINTH_IRQ_BASE && i <= GIUINTH_IRQ_LAST)
			irq_desc[i].handler = &giuinth_irq_type;
	}

	for (i = 0; i < 32; i++) {
		vr41xx_irqcascade[i].cascade = 0;
		vr41xx_irqcascade[i].get_irq_number = no_cascade_get_irq_number;
	}

}

void __init init_IRQ(void)
{

	memset(irq_desc, 0, sizeof(irq_desc));

	init_generic_irq();
	mips_cpu_irq_init(MIPS_CPU_IRQ_BASE);
	vr41xx_icu_irq_init();
	vr41xx_board_irq_init();

	setup_irq(ICU_IRQ, &cascade);
	setup_irq(GIU_IRQ, &cascade);

	set_except_vector(0, vr41xx_handle_interrupt);

#ifdef CONFIG_REMOTE_DEBUG
	printk("Setting debug traps - please connect the remote debugger.\n");
	set_debug_traps();
	breakpoint();
#endif
}

/*=======================================================================*/

static void giuint_do_IRQ(int giuint_irq, struct pt_regs *regs)
{
	struct irqcascade *irq;
	int cascade_irq;

	irq = &vr41xx_irqcascade[giuint_irq - GIUINTL_IRQ_BASE];
	if (irq->cascade) {
		cascade_irq = irq->get_irq_number(giuint_irq);
		disable_irq(giuint_irq);
		if (cascade_irq > 0)
			do_IRQ(cascade_irq, regs);
		enable_irq(giuint_irq);
	}
	else
		do_IRQ(giuint_irq, regs);
}

static inline void giuint_irqdispatch(u16 pendl, u16 pendh, struct pt_regs *regs)
{
	int i;

	if (pendl) {
		for (i = 0; i < 16; i++) {
			if (pendl & (0x0001 << i)) {
				giuint_do_IRQ(GIUINTL_IRQ_BASE + i, regs);
				return;
			}
		}
	}
	else if (pendh) {
		for (i = 0; i < 16; i++) {
			if (pendh & (0x0001 << i)) {
				giuint_do_IRQ(GIUINTH_IRQ_BASE + i, regs);
				return;
			}
		}
	}
}

asmlinkage void icu_irqdispatch(struct pt_regs *regs)
{
	u16 pend1, pend2, pendl, pendh;
	u16 mask1, mask2, maskl, maskh;
	int i;

	pend1 = readw(SYSINT1REG);
	mask1 = readw(MSYSINT1REG);

	pend2 = readw(SYSINT2REG);
	mask2 = readw(MSYSINT2REG);

	pendl = readw(GIUINTLREG);
	maskl = readw(MGIUINTLREG);

	pendh = readw(GIUINTHREG);
	maskh = readw(MGIUINTHREG);

	pend1 &= mask1;
	pend2 &= mask2;
	pendl &= maskl;
	pendh &= maskh;

	if (pend1) {
		if ((pend1 & 0x01ff) == 0x0100) {
			giuint_irqdispatch(pendl, pendh, regs);
		}
		else {
			for (i = 0; i < 16; i++) {
				if (pend1 & (0x0001 << i)) {
					do_IRQ(SYSINT1_IRQ_BASE + i, regs);
					break;
				}
			}
		}
		return;
	}
	else if (pend2) {
		for (i = 0; i < 16; i++) {
			if (pend2 & (0x0001 << i)) {
				do_IRQ(SYSINT2_IRQ_BASE + i, regs);
				break;
			}
		}
	}
}
