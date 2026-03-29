/* Minimal framebuffer test for BE-300 (240x320 16bpp RGB565) */
/* No libc - direct MIPS-II asm syscalls */

void _start(void) __attribute__((section(".text")));

void _start(void) {
    /* Inline asm to write "X\n" to stdout (fd 1) as the simplest possible test */
    /* write(1, buf, 2) = syscall 4004(1, buf, 2) */
    __asm__ volatile(
        ".set noreorder\n"
        /* First: write 'X\n' to serial (fd 1) */
        "li   $v0, 4004\n"      /* SYS_write */
        "li   $a0, 1\n"         /* fd = stdout */
        "la   $a1, msg\n"       /* buf */
        "li   $a2, 25\n"        /* count */
        "syscall\n"
        "nop\n"

        /* open /dev/fb0 */
        "li   $v0, 4005\n"      /* SYS_open */
        "la   $a0, devfb\n"     /* path */
        "li   $a1, 2\n"         /* O_RDWR */
        "li   $a2, 0\n"
        "syscall\n"
        "nop\n"

        /* Check result - if a3 != 0, open failed */
        "bnez $a3, fail\n"
        "move $s0, $v0\n"       /* s0 = fd (delay slot) */

        /* Write "fb ok\n" */
        "li   $v0, 4004\n"
        "li   $a0, 1\n"
        "la   $a1, msg2\n"
        "li   $a2, 6\n"
        "syscall\n"
        "nop\n"

        /* Fill framebuffer: write 240*2=480 bytes per row, 320 rows */
        /* Use red (0xF800) for all pixels */
        "li   $s1, 0\n"         /* y counter */
        "li   $s2, 320\n"       /* max rows */

        "rowloop:\n"
        /* Fill line buffer on stack with red pixels */
        "addiu $sp, $sp, -480\n"
        "move  $t0, $sp\n"
        "li    $t1, 240\n"      /* pixel count */
        "li    $t2, 0x00F8\n"   /* 0xF800 as bytes: little-endian = 0x00, 0xF8 */
        "pixloop:\n"
        "sh    $t2, 0($t0)\n"   /* store halfword - but 0x00F8 not 0xF800 */
        "addiu $t0, $t0, 2\n"
        "addiu $t1, $t1, -1\n"
        "bnez  $t1, pixloop\n"
        "nop\n"

        /* write(fd, sp, 480) */
        "li   $v0, 4004\n"
        "move $a0, $s0\n"       /* fd */
        "move $a1, $sp\n"       /* buf */
        "li   $a2, 480\n"       /* count */
        "syscall\n"
        "nop\n"

        "addiu $sp, $sp, 480\n" /* restore stack */
        "addiu $s1, $s1, 1\n"
        "bne  $s1, $s2, rowloop\n"
        "nop\n"

        /* Write "done\n" */
        "li   $v0, 4004\n"
        "li   $a0, 1\n"
        "la   $a1, msg3\n"
        "li   $a2, 13\n"
        "syscall\n"
        "nop\n"

        /* infinite loop */
        "loop: b loop\n"
        "nop\n"

        "fail:\n"
        "li   $v0, 4004\n"
        "li   $a0, 1\n"
        "la   $a1, msg4\n"
        "li   $a2, 15\n"
        "syscall\n"
        "nop\n"
        "b    loop\n"
        "nop\n"

        /* String data */
        ".data\n"
        "msg:  .asciiz \"fbtest: hello serial!\\n\\0\\0\\0\"\n"
        "devfb: .asciiz \"/dev/fb0\"\n"
        "msg2: .asciiz \"fb ok\\n\"\n"
        "msg3: .asciiz \"fbtest: done\\n\"\n"
        "msg4: .asciiz \"fb open fail\\n\\0\"\n"
        ".text\n"
        ".set reorder\n"
    );
}
