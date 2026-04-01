/*
 * FILE NAME
 *	arch/mips/vr41xx/vr4181/common/icu.h
 *
 * BRIEF MODULE DESCRIPTION
 *	Include file for Interrupt Control Unit of the NEC VR4181.
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
 *  Paul Mundt <lethal@chaoticdreams.org>
 *  - Modified this code for VR4181 support.
 *
 *  MontaVista Software Inc. <yyuasa@mvista.com> or <source@mvista.com>
 *  - New creation, NEC VR4122 and VR4131 are supported.
 *
 *  Yoichi Yuasa <yuasa@hh.iij4u.or.jp>  Tue,  5 Mar 2002
 *  -  Modified this code for VR4121 support.
 *
 *  Yoichi Yuasa <yuasa@hh.iij4u.or.jp>  Tue,  5 Mar 2002
 *  - This file was copied from arch/mips/vr41xx/vr4121/common/icu.h.
 *    Only FILE NAME and BRIEF MODULE DESCRIPTION modified this file.
 */
#ifndef __VR41XX_ICU_H
#define __VR41XX_ICU_H

#include <linux/config.h>
#include <asm/addrspace.h>

#define SYSINT1REG	KSEG1ADDR(0x0a000080)
#define GIUINTLREG	KSEG1ADDR(0x0a000088)
#define MSYSINT1REG	KSEG1ADDR(0x0a00008c)
#define MGIUINTLREG	KSEG1ADDR(0x0a000094)

#define NMIREG		KSEG1ADDR(0x0a000098)
#define SOFTINTREG	KSEG1ADDR(0x0a00009a)

#define SYSINT2REG	KSEG1ADDR(0x0a000200)
#define GIUINTHREG	KSEG1ADDR(0x0a000202)
#define MSYSINT2REG	KSEG1ADDR(0x0a000206)
#define MGIUINTHREG	KSEG1ADDR(0x0a000208)

#define GIUINTSTATL	KSEG1ADDR(0x0b000108)
#define GIUINTSTATH	KSEG1ADDR(0x0b00010a)
#define GIUINTENL	KSEG1ADDR(0x0b00010c)
#define GIUINTENH	KSEG1ADDR(0x0b00010e)
#define GIUINTTYPL	KSEG1ADDR(0x0b000110)
#define GIUINTTYPH	KSEG1ADDR(0x0b000112)
#define GIUINTALSELL	KSEG1ADDR(0x0b000114)
#define GIUINTALSELH	KSEG1ADDR(0x0b000116)
#define GIUINTHTSELL	KSEG1ADDR(0x0b000118)
#define GIUINTHTSELH	KSEG1ADDR(0x0b00011a)

#define MIPS_CPU_IRQ_BASE	0
#define SYSINT1_IRQ_BASE	8
#define SYSINT1_IRQ_LAST	23
#define SYSINT2_IRQ_BASE	24
#define SYSINT2_IRQ_LAST	39
#define GIUINTL_IRQ_BASE	40
#define GIUINTL_IRQ_LAST	55
#define GIUINTH_IRQ_BASE	56
#define GIUINTH_IRQ_LAST	71

#define ICU_IRQ			2
#define GIU_IRQ			(SYSINT1_IRQ_BASE + 8)

#endif /* __VR41XX_ICU_H */
