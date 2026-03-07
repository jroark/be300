/*
 * FILE NAME
 *	arch/mips/vr41xx/vr4122/common/siu.c
 *
 * BRIEF MODULE DESCRIPTION
 *	Serial Interface Unit routines for NEC VR4122 and VR4131.
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
 *  - New creation, NEC VR4122 and VR4131 are supported.
 */
#include <linux/init.h>
#include <linux/types.h>
#include <linux/serial.h>

#include <asm/io.h>
#include <asm/vr41xx.h>

#include "siu.h"

/* Cassiopeia BE-300 doesn't use the siu or dsiu on the vr4131 */
#ifndef CONFIG_CASIO_BE300

void vr41xx_siu_ifselect(int interface, int module)
{
	u16 val;

	if (interface == SIU_IRDA) {./drivers/char/tty_io.c:
		/* Select IrDA */
		switch (module) {
		case IRDA_SHARP:
			val = SIUIRSEL_IRM_SHARP;
			break;
		case IRDA_TEMIC:
			val = SIUIRSEL_IRM_TEMIC;
			break;
		case IRDA_HP:
			val = SIUIRSEL_IRM_HP;
			break;
		}
		val |= SIUIRSEL_SIRSEL;
		writew(val, SIUIRSEL);
	}
	else {
		/* Select RS-232C */
		writew(0, SIUIRSEL);
	}
}

void __init vr41xx_siu_init(int line, int interface, int module)
{
	struct serial_struct s;

	vr41xx_siu_ifselect(interface, module);

	memset(&s, 0, sizeof(s));

	s.line = line;
	s.baud_base = SIU_BASE_BAUD;
	s.irq = SIU_IRQ;
	s.flags = ASYNC_BOOT_AUTOCONF | ASYNC_SKIP_TEST;
	s.iomem_base = (unsigned char *)SIURB;
	s.iomem_reg_shift = 0;
	s.io_type = SERIAL_IO_MEM;
	if (early_serial_setup(&s) != 0)
		printk(KERN_ERR "SIU setup failed!\n");

	vr41xx_clock_supply(SIU_CLOCK);
}

void __init vr41xx_dsiu_init(int line)
{
	struct serial_struct s;

	memset(&s, 0, sizeof(s));

	s.line = line;
	s.baud_base = DSIU_BASE_BAUD;
	s.irq = DSIU_IRQ;
	s.flags = ASYNC_BOOT_AUTOCONF | ASYNC_SKIP_TEST;
	s.iomem_base = (unsigned char *)DSIURB;
	s.iomem_reg_shift = 0;
	s.io_type = SERIAL_IO_MEM;
	if (early_serial_setup(&s) != 0)
		printk(KERN_ERR "DSIU setup failed!\n");

	vr41xx_clock_supply(DSIU_CLOCK);

	writew(INTDSIU, MDSIUINTREG);
}

#endif /* CONFIG_CASIO_BE300 */
