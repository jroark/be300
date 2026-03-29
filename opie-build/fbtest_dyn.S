/* Minimal framebuffer test - MIPS-II, disguised as dynamic ELF */
/* Uses raw syscalls, but includes .interp section so kernel uses */
/* the dynamic loader path (which makes the ELF loader happy) */
#include <asm/regdef.h>
#include <asm/unistd.h>

	.section .interp,"a"
	.asciz "/lib/ld-uClibc.so.0"

	.set noreorder
	.text
	.globl _start
	.globl __start
	.ent _start
__start:
_start:
	/* write(1, msg, 14) - announce on serial */
	li	v0, __NR_write
	li	a0, 1
	la	a1, msg
	li	a2, 14
	syscall
	nop

	/* open("/dev/fb0", O_RDWR) */
	li	v0, __NR_open
	la	a0, devfb
	li	a1, 2		/* O_RDWR */
	li	a2, 0
	syscall
	nop
	bnez	a3, open_fail
	move	s0, v0		/* s0 = fd */

	/* write(1, msg_ok, 16) */
	li	v0, __NR_write
	li	a0, 1
	la	a1, msg_ok
	li	a2, 16
	syscall
	nop

	/* Draw 320 rows of red (0xF800) pixels */
	addiu	sp, sp, -480	/* line buffer on stack */
	move	t0, sp
	li	t1, 240
	li	t2, 0xF800
fill:	sh	t2, 0(t0)
	addiu	t0, t0, 2
	addiu	t1, t1, -1
	bnez	t1, fill
	nop

	li	s1, 0
	li	s2, 320
rowloop:
	li	v0, __NR_write
	move	a0, s0
	move	a1, sp
	li	a2, 480
	syscall
	nop
	addiu	s1, s1, 1
	bne	s1, s2, rowloop
	nop

	addiu	sp, sp, 480

	li	v0, __NR_write
	li	a0, 1
	la	a1, msg_done
	li	a2, 13
	syscall
	nop

1:	b	1b
	nop

open_fail:
	li	v0, __NR_write
	li	a0, 1
	la	a1, msg_fail
	li	a2, 14
	syscall
	nop
	b	1b
	nop
	.end _start

	.rdata
msg:		.ascii "fbtest: start\n"
devfb:		.asciiz "/dev/fb0"
msg_ok:		.ascii "fbtest: drawing\n"
msg_done:	.ascii "fbtest: done\n"
msg_fail:	.ascii "fb open fail!\n"
