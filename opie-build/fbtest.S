/* Minimal framebuffer test - pure MIPS-II assembly */
/* Draws red to /dev/fb0 (240x320 16bpp) */
#include <asm/regdef.h>
#include <asm/unistd.h>

	.set noreorder
	.text
	.globl _start
	.ent _start
_start:
	/* write(1, msg, 22) - announce on serial */
	li	v0, __NR_write
	li	a0, 1
	la	a1, msg
	li	a2, 22
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

	/* write(1, msg_ok, 6) */
	li	v0, __NR_write
	li	a0, 1
	la	a1, msg_ok
	li	a2, 6
	syscall
	nop

	/* Draw 320 rows of red (0xF800) pixels */
	addiu	sp, sp, -480	/* line buffer on stack */
	/* Fill buffer with red pixels */
	move	t0, sp
	li	t1, 240
	li	t2, 0xF800
fill:	sh	t2, 0(t0)
	addiu	t0, t0, 2
	addiu	t1, t1, -1
	bnez	t1, fill
	nop

	/* Write 320 rows */
	li	s1, 0
	li	s2, 320
rowloop:
	li	v0, __NR_write
	move	a0, s0		/* fd */
	move	a1, sp		/* buf */
	li	a2, 480		/* 240 * 2 bytes */
	syscall
	nop
	addiu	s1, s1, 1
	bne	s1, s2, rowloop
	nop

	addiu	sp, sp, 480

	/* write(1, msg_done, 13) */
	li	v0, __NR_write
	li	a0, 1
	la	a1, msg_done
	li	a2, 13
	syscall
	nop

	/* infinite loop */
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
msg:		.asciiz "fbtest: starting...\n\0\0"
devfb:		.asciiz "/dev/fb0"
msg_ok:		.asciiz "fb ok\n"
msg_done:	.asciiz "fbtest: done\n"
msg_fail:	.asciiz "fb open fail\n"
