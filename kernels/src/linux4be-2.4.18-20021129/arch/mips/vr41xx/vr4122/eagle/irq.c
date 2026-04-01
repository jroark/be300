/*
 * FILE NAME
 *	arch/mips/vr41xx/vr4122/eagle/irq.c
 *
 * BRIEF MODULE DESCRIPTION
 *	Interrupt routines for the NEC Eagle/Hawk board.
 *
 * Author: Yoichi Yuasa
 *         yyuasa@mvista.com or source@mvista.com
 *
 * Copyright 2002 MontaVista Software Inc.
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
 *  MontaVista Software Inc. <yyuasa@mvista.com> or <source@mvista.com>
 *  - Added support for NEC Hawk.
 *
 *  MontaVista Software Inc. <yyuasa@mvista.com> or <source@mvista.com>
 *  - New creation, NEC Eagle is supported.
 */
#include <linux/init.h>
#include <linux/interrupt.h>

#include <asm/io.h>
#include <asm/vr41xx/eagle.h>

static void enable_pciint_irq(unsigned int irq)
{
	u8 val;

	val = readb(NEC_EAGLE_PCIINTMSKREG);
	val |= (u8)1 << (irq - PCIINT_IRQ_BASE);
	writeb(val, NEC_EAGLE_PCIINTMSKREG);
}

static void disable_pciint_irq(unsigned int irq)
{
	u8 val;

	val = readb(NEC_EAGLE_PCIINTMSKREG);
	val &= ~((u8)1 << (irq - PCIINT_IRQ_BASE));
	writeb(val, NEC_EAGLE_PCIINTMSKREG);
}

static unsigned int startup_pciint_irq(unsigned int irq)
{
	enable_pciint_irq(irq);
	return 0; /* never anything pending */
}

#define shutdown_pciint_irq	disable_pciint_irq
#define ack_pciint_irq		disable_pciint_irq

static void end_pciint_irq(unsigned int irq)
{
	if (!(irq_desc[irq].status & (IRQ_DISABLED | IRQ_INPROGRESS)))
		enable_pciint_irq(irq);
}

static struct hw_interrupt_type pciint_irq_type = {
	"PCIINT",
	startup_pciint_irq,
	shutdown_pciint_irq,
       	enable_pciint_irq,
	disable_pciint_irq,
	ack_pciint_irq,
	end_pciint_irq,
	NULL
};

static int pciint_get_irq_number(int irq)
{
	u8 val;
	int i;

	val = readb(NEC_EAGLE_PCIINTREG);
	val &= (NEC_EAGLE_PCIINT_CP_INTA | NEC_EAGLE_PCIINT_CP_INTB |
	        NEC_EAGLE_PCIINT_CP_INTC | NEC_EAGLE_PCIINT_CP_INTD |
	        NEC_EAGLE_PCIINT_LANINT);

	for (i = 0; i < 5; i++)
		if (val & (0x01 << i))
			return PCIINT_IRQ_BASE + i;

	return -EINVAL;
}

void __init eagle_irq_init(void)
{
	int i;

	writeb(0, NEC_EAGLE_PCIINTMSKREG);

	vr41xx_set_irq_trigger(VRC4173_PIN, TRIGGER_LEVEL, SIGNAL_THROUGH);
	vr41xx_set_irq_level(VRC4173_PIN, LEVEL_LOW);

	vr41xx_set_irq_trigger(PCISLOT_PIN, TRIGGER_LEVEL, SIGNAL_THROUGH);
	vr41xx_set_irq_level(PCISLOT_PIN, LEVEL_HIGH);

	vr41xx_set_irq_trigger(PCIINT_PIN, TRIGGER_LEVEL, SIGNAL_THROUGH);
	vr41xx_set_irq_level(PCIINT_PIN, LEVEL_HIGH);

	vr41xx_set_irq_trigger(DCD_PIN, TRIGGER_EDGE, SIGNAL_HOLD);
	vr41xx_set_irq_level(DCD_PIN, LEVEL_LOW);

	for (i = PCIINT_IRQ_BASE; i <= PCIINT_IRQ_LAST; i++)
		irq_desc[i].handler = &pciint_irq_type;

	vr41xx_cascade_irq(PCIINT_IRQ, pciint_get_irq_number);
}
