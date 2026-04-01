/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1994, 1995 Waldorf Electronics
 * Written by Ralf Baechle and Andreas Busse
 * Copyright (C) 1995 - 1999 Ralf Baechle
 * Copyright (C) 1996 Paul M. Antoine
 * Modified for DECStation and hence R3000 support by Paul M. Antoine
 * Further modifications by David S. Miller and Harald Koerfgen
 * Copyright (C) 1999 Silicon Graphics, Inc.
 *
 * Kevin Kissell, kevink@mips.com and Carsten Langgaard, carstenl@mips.com
 * Copyright (C) 2000 MIPS Technologies, Inc.  All rights reserved.
 */
#include <linux/config.h>
#include <linux/init.h>
#include <linux/threads.h>

#include <asm/asm.h>
#include <asm/current.h>
#include <asm/offset.h>
#include <asm/pgtable-bits.h>
#include <asm/processor.h>
#include <asm/regdef.h>
#include <asm/cachectl.h>
#include <asm/mipsregs.h>
#include <asm/stackframe.h>
#ifdef CONFIG_PM_SUSPEND_WAKEUP
#include <asm/power.h>
#endif

		__INIT

		.text
		/*
		 * Reserved space for exception handlers.
		 * Necessary for machines which link their kernels at KSEG0.
		 */
		.fill	0x280

		/* Cache Error */
		LEAF(except_vec2_generic)
		.set	noreorder
		.set	noat
		.set    mips0
		/*
		 * This is a very bad place to be.  Our cache error
		 * detection has triggered.  If we have write-back data
		 * in the cache, we may not be able to recover.  As a
		 * first-order desperate measure, turn off KSEG0 cacheing.
		 */
		mfc0	k0,CP0_CONFIG
		li	k1,~CONF_CM_CMASK
		and	k0,k0,k1
		ori	k0,k0,CONF_CM_UNCACHED
		mtc0	k0,CP0_CONFIG
		/* Give it a few cycles to sink in... */
		nop
		nop
		nop

		j	cache_parity_error
		nop
		END(except_vec2_generic)

		.set	at

		/*
		 * Special interrupt vector for embedded MIPS.  This is a
		 * dedicated interrupt vector which reduces interrupt processing
		 * overhead.  The jump instruction will be inserted here at
		 * initialization time.  This handler may only be 8 bytes in
		 * size!
		 */
		NESTED(except_vec4, 0, sp)
1:		j	1b			/* Dummy, will be replaced */
		 nop
		END(except_vec4)

		/*
		 * SBDDP EJTAG debug exception handler.
		 * The EJTAG debug exception entry point is 0xbfc00480, which
		 * normally is in the boot PROM, so the boot PROM must do a
		 * unconditional jump to this vector.
		 */	
		NESTED(except_vec_ejtag_debug, 0, sp)
		j	ejtag_debug_handler
		 nop
		END(except_vec_ejtag_debug)

		/*
		 * EJTAG debug exception handler.
		 */
		NESTED(ejtag_debug_handler, PT_SIZE, sp)
		.set	noat
		.set	noreorder
		SAVE_ALL
		PRINT("SDBBP EJTAG debug exception - not handled yet, just ignored!\n");
		mfc0	k0, $23         # Get EJTAG Debug register.
		mfc0    k1, $24         # Get DEPC register.
		bgez	k0, 1f
		 addiu	k1, k1, 4	# SBDDP inst. in delay slot.
		addiu	k1, k1, 4
1:		mtc0	k1, $24
		RESTORE_ALL
		.word	0x4200001f      # deret, return EJTAG debug exception.
		 nop
		.set	at
		END(ejtag_debug_handler)

#ifdef CONFIG_CPU_LX45XXX
/*
 * Lexra specific vectored interrupt handlers
 */
		.org	0x700
		NESTED(lx_int_vec8, 0, sp)
		j	lx_vec_handle_int
		nop
		END(lx_int_vec8)

		NESTED(lx_int_vec9, 0, sp)
		j	lx_vec_handle_int
		nop
		END(lx_int_vec9)	

		NESTED(lx_int_vec10, 0, sp)
		j	lx_vec_handle_int
		nop
		END(lx_int_vec10)	

		NESTED(lx_int_vec11, 0, sp)
		j	lx_vec_handle_int
		nop
		END(lx_int_vec11)	

		NESTED(lx_int_vec12, 0, sp)
		j	lx_vec_handle_int
		nop
		END(lx_int_vec12)	

		NESTED(lx_int_vec13, 0, sp)
		j	lx_vec_handle_int
		nop
		END(lx_int_vec13)	

		NESTED(lx_int_vec14, 0, sp)
		j	lx_vec_handle_int
		nop
		END(lx_int_vec14)	

		NESTED(lx_int_vec15, 0, sp)
		j	lx_vec_handle_int
		nop
		END(lx_int_vec15)	
 
 	.org	0x800
#endif		


		/*
		 * Kernel entry point
		 */
NESTED(kernel_entry, 16, sp)
		.set	noreorder
		/* The following two symbols are used for kernel profiling. */
		EXPORT(stext)
		EXPORT(_stext)

#ifdef CONFIG_PM_SUSPEND_WAKEUP
	.set	reorder

	// Check if we were hibernating.
	lw	t0, hibernation_state
	la	t1, HIB_MAGIC
	la	t2, RUN_MAGIC
	sw	t2, hibernation_state
	bne	t0, t1, end_check_hibernating

	.set	noreorder
	.set	mips3

	// Appears that we were hibernating, so wake up.

	// Jump to KESG1 here instead of assuming that the bootloader
	// already has us there.
	la	t0, 1f
	li	t1, 0x1fffffff
	and	t0, t1
	li	t1, 0xa0000000
	or	t0, t1
	jr	t0
	 nop
1:
	
	// Don't trust the bootloader to invalidate the caches.

	// Invalidate the instruction cache.
	mtc0	zero, CP0_TAGLO
	li	s0, 0x80000000
	li	s1, 0x80000000 + (1 << 14) // 16k
1:
	cache	(2 << 2) | 0, (s0)
	add	s0, 0x10
	bne	s0, s1, 1b
	 nop

	// Invalidate the data cache.
	mtc0	zero, CP0_TAGLO
	li	s0, 0x80000000
	li	s1, 0x80000000 + (1 << 13) // 8k
1:
	cache	(2 << 2) | 1, (s0)
	add	s0, 0x10
	bne	s0, s1, 1b
	 nop

	.set	reorder

	// Jump to linked segment.
	la	t0, 1f
	jr	t0
1:

	// Restore the stack and do the wakeup sequence.
	lw	sp, kernelsp
	subu	sp, PT_SIZE
	ori	$28, sp, 0x1fff;
	xori	$28, 0x1fff;
	jal	do_wakeup

	bnez	v0, end_check_hibernating

	j	ret_from_hib_wakeup

end_check_hibernating:
	.set	noreorder
#endif

#ifdef CONFIG_XIP_ROM
	/*
	 * For xip from rom, copy initialized data from end of text in rom
	 * to _fdata though _edata in ram.
	 */
	la	s0, _etext
	la	s1, _fdata
	la	s2, _edata
1:
	lw	t0, (s0)
	sw	t0, (s1)
	addi	s1, 4
	bne	s1, s2, 1b
	 addi	s0, 4
#endif

		/*
		 * Stack for kernel and init, current variable
		 */
		la	$28, init_task_union
		addiu	t0, $28, KERNEL_STACK_SIZE-32
		subu	sp, t0, 4*SZREG

		sw	t0, kernelsp
		/* The firmware/bootloader passes argc/argp/envp
		 * to us as arguments.  But clear bss first because
		 * the romvec and other important info is stored there
		 * by prom_init().
		 */
		la	t0, _edata
		sw	zero, (t0)
		la	t1, (_end - 4)
1:
		addiu	t0, 4
		bne	t0, t1, 1b
		 sw	zero, (t0)

		jal	init_arch
		 nop
		END(kernel_entry)


#ifdef CONFIG_SMP

/*
 * SMP slave cpus entry point.  Board specific code for bootstrap calls this
 * function after setting up the stack and gp registers. 
 */
		LEAF(smp_bootstrap)
		.set push
		.set noreorder
		mtc0	zero, CP0_WIRED
		CLI
		mfc0	t0, CP0_STATUS
		li	t1, ~(ST0_CU1|ST0_CU2|ST0_CU3|ST0_KX|ST0_SX)
		and	t0, t1
		or	t0, (ST0_CU0|ST0_FR);
		jal	start_secondary
		mtc0	t0, CP0_STATUS
		.set pop
		END(smp_bootstrap)
#endif

		__FINIT

		.comm	kernelsp,    NR_CPUS * 8, 8
		.comm	pgd_current, NR_CPUS * 8, 8

	.macro	page name, order=0
	.globl	\name
\name:	.size	\name, (_PAGE_SIZE << \order)
	.org	. + (_PAGE_SIZE << \order)
	.type	\name, @object
	.endm

	.data
	.align	12

	page	swapper_pg_dir, _PGD_ORDER
	page	empty_bad_page, 0
	page	empty_bad_page_table, 0
	page	invalid_pte_table, 0
