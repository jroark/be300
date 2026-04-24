.set noreorder

/*
 * Raw .text replacement for a WinCE XIP stream-driver module.
 *
 * This is assembled as a flat binary for the target module's existing .text
 * section, not as a standalone PE. The surrounding XIP metadata, exports, and
 * imports remain in place; we only replace code bytes up to the import table.
 *
 * Stub offsets below are relative to the start of the target .text section.
 */

    .text
    .org 0x0000

dllmain_impl:
    addiu   $v0, $zero, 1
    jr      $ra
    nop

bool_true_impl:
    addiu   $v0, $zero, 1
    jr      $ra
    nop

zero_impl:
    addu    $v0, $zero, $zero
    jr      $ra
    nop

void_impl:
    jr      $ra
    nop

open_impl:
    addiu   $v0, $zero, 1
    jr      $ra
    nop

spin_delay_impl:
    lui     $t0, 0x0004
    ori     $t0, $t0, 0x0000
1:
    addiu   $t0, $t0, -1
    bnez    $t0, 1b
    nop
    jr      $ra
    nop

uart_putc:
    lui     $t0, 0xAA00
    ori     $t0, $t0, 0x8680
2:
    lbu     $t1, 0x14($t0)
    andi    $t1, $t1, 0x20
    beqz    $t1, 2b
    nop
    sb      $a0, 0($t0)
    jr      $ra
    nop

uart_init:
    lui     $t0, 0xAA00
    ori     $t0, $t0, 0x8680
    lui     $t2, 0xAA00
    ori     $t2, $t2, 0x8000
    lui     $t3, 0x0000
    ori     $t3, $t3, 0x200C
    sw      $t3, 0x00($t2)
    lui     $t3, 0x0000
    ori     $t3, $t3, 0x1100
    sw      $t3, 0x04($t2)
    addiu   $t3, $zero, 12
    sw      $t3, 0x10($t2)
    addiu   $t3, $zero, 2
    sw      $t3, 0x14($t2)
    addiu   $t3, $zero, 10
    sw      $t3, 0x40($t2)
    lui     $t2, 0xAF00
    lhu     $t3, 0x0140($t2)
    ori     $t3, $t3, 0x20
    sh      $t3, 0x0140($t2)
    lhu     $t3, 0x0144($t2)
    ori     $t3, $t3, 0x20
    sh      $t3, 0x0144($t2)
    addiu   $t1, $zero, 0x80
    sb      $t1, 0x0C($t0)
    addiu   $t1, $zero, 10
    sb      $t1, 0x00($t0)
    sb      $zero, 0x04($t0)
    addiu   $t1, $zero, 0x03
    sb      $t1, 0x0C($t0)
    addiu   $t1, $zero, 0x07
    sb      $t1, 0x08($t0)
    addiu   $t1, $zero, 0x03
    sb      $t1, 0x10($t0)
    jr      $ra
    nop

uart_puts:
    addiu   $sp, $sp, -16
    sw      $ra, 12($sp)
    addu    $t2, $a0, $zero
3:
    lbu     $t3, 0($t2)
    beqz    $t3, 4f
    addiu   $t2, $t2, 1
    addu    $a0, $t3, $zero
    bal     uart_putc
    nop
    b       3b
    nop
4:
    lw      $ra, 12($sp)
    addiu   $sp, $sp, 16
    jr      $ra
    nop

uart_crlf:
    addiu   $sp, $sp, -16
    sw      $ra, 12($sp)
    addiu   $a0, $zero, 13
    bal     uart_putc
    nop
    addiu   $a0, $zero, 10
    bal     uart_putc
    nop
    lw      $ra, 12($sp)
    addiu   $sp, $sp, 16
    jr      $ra
    nop

uart_puthex32:
    addiu   $sp, $sp, -24
    sw      $ra, 20($sp)
    sw      $a0, 16($sp)
    addiu   $t3, $zero, 8
    bal     5f
    nop
5:
    addiu   $t6, $ra, hex_digits - 5b
6:
    lw      $t2, 16($sp)
    srl     $t4, $t2, 28
    addu    $t5, $t6, $t4
    lbu     $a0, 0($t5)
    bal     uart_putc
    nop
    lw      $t2, 16($sp)
    sll     $t2, $t2, 4
    sw      $t2, 16($sp)
    addiu   $t3, $t3, -1
    bnez    $t3, 6b
    nop
    lw      $ra, 20($sp)
    addiu   $sp, $sp, 24
    jr      $ra
    nop

dump_word_at:
    addiu   $sp, $sp, -24
    sw      $ra, 20($sp)
    sw      $a1, 16($sp)
    bal     uart_puts
    nop
    lw      $t0, 16($sp)
    lw      $a0, 0($t0)
    bal     uart_puthex32
    nop
    bal     uart_crlf
    nop
    lw      $ra, 20($sp)
    addiu   $sp, $sp, 24
    jr      $ra
    nop

cex_init_impl:
    addiu   $sp, $sp, -32
    sw      $ra, 28($sp)
    sw      $a0, 24($sp)

    bal     uart_init
    nop

    bal     10f
    nop
10:
    addiu   $a0, $ra, banner_msg - 10b
    bal     uart_puts
    nop
    bal     uart_crlf
    nop

    bal     11f
    nop
11:
    addiu   $a0, $ra, ctx_msg - 11b
    bal     uart_puts
    nop
    lw      $a0, 24($sp)
    bal     uart_puthex32
    nop
    bal     uart_crlf
    nop

    bal     12f
    nop
12:
    addiu   $a0, $ra, phase_init_msg - 12b
    bal     uart_puts
    nop
    bal     uart_crlf
    nop

    bal     20f
    nop
20:
    addiu   $a0, $ra, msg_a0002200 - 20b
    lui     $a1, 0xA000
    ori     $a1, $a1, 0x2200
    bal     dump_word_at
    nop

    bal     21f
    nop
21:
    addiu   $a0, $ra, msg_a0002220 - 21b
    lui     $a1, 0xA000
    ori     $a1, $a1, 0x2220
    bal     dump_word_at
    nop

    bal     22f
    nop
22:
    addiu   $a0, $ra, msg_a0002228 - 22b
    lui     $a1, 0xA000
    ori     $a1, $a1, 0x2228
    bal     dump_word_at
    nop

    bal     23f
    nop
23:
    addiu   $a0, $ra, msg_a0002274 - 23b
    lui     $a1, 0xA000
    ori     $a1, $a1, 0x2274
    bal     dump_word_at
    nop

    bal     24f
    nop
24:
    addiu   $a0, $ra, msg_aa000c30 - 24b
    lui     $a1, 0xAA00
    ori     $a1, $a1, 0x0C30
    bal     dump_word_at
    nop

    bal     25f
    nop
25:
    addiu   $a0, $ra, msg_aa000c34 - 25b
    lui     $a1, 0xAA00
    ori     $a1, $a1, 0x0C34
    bal     dump_word_at
    nop

    bal     26f
    nop
26:
    addiu   $a0, $ra, msg_aa000c38 - 26b
    lui     $a1, 0xAA00
    ori     $a1, $a1, 0x0C38
    bal     dump_word_at
    nop

    bal     27f
    nop
27:
    addiu   $a0, $ra, msg_aa000c48 - 27b
    lui     $a1, 0xAA00
    ori     $a1, $a1, 0x0C48
    bal     dump_word_at
    nop

    bal     28f
    nop
28:
    addiu   $a0, $ra, msg_aa000c4c - 28b
    lui     $a1, 0xAA00
    ori     $a1, $a1, 0x0C4C
    bal     dump_word_at
    nop

    bal     29f
    nop
29:
    addiu   $a0, $ra, msg_af000080 - 29b
    lui     $a1, 0xAF00
    ori     $a1, $a1, 0x0080
    bal     dump_word_at
    nop

    bal     30f
    nop
30:
    addiu   $a0, $ra, msg_af000100 - 30b
    lui     $a1, 0xAF00
    ori     $a1, $a1, 0x0100
    bal     dump_word_at
    nop

    bal     spin_delay_impl
    nop

    bal     31f
    nop
31:
    addiu   $a0, $ra, phase_afterspin_msg - 31b
    bal     uart_puts
    nop
    bal     uart_crlf
    nop

    bal     32f
    nop
32:
    addiu   $a0, $ra, msg_aa000c30 - 32b
    lui     $a1, 0xAA00
    ori     $a1, $a1, 0x0C30
    bal     dump_word_at
    nop

    bal     33f
    nop
33:
    addiu   $a0, $ra, msg_aa000c34 - 33b
    lui     $a1, 0xAA00
    ori     $a1, $a1, 0x0C34
    bal     dump_word_at
    nop

    bal     34f
    nop
34:
    addiu   $a0, $ra, msg_aa000c38 - 34b
    lui     $a1, 0xAA00
    ori     $a1, $a1, 0x0C38
    bal     dump_word_at
    nop

    addiu   $v0, $zero, 1
    lw      $ra, 28($sp)
    addiu   $sp, $sp, 32
    jr      $ra
    nop

    .org 0x045C
cex_close_stub:
    b       bool_true_impl
    nop

    .org 0x05B8
dllmain_stub:
    b       dllmain_impl
    nop

    .org 0x0C38
cex_powerdown_stub:
    b       void_impl
    nop

    .org 0x0C4C
cex_powerup_stub:
    b       void_impl
    nop

    .org 0x0C54
cex_rw_stub:
    b       zero_impl
    nop

    .org 0x0F44
cex_ioctl_stub:
    b       bool_true_impl
    nop

    .org 0x10C8
cex_open_stub:
    b       open_impl
    nop

    .org 0x10D0
cex_deinit_stub:
    b       bool_true_impl
    nop

    .org 0x1104
cex_init_stub:
    b       cex_init_impl
    nop

banner_msg:
    .asciiz "[EARLYDIAG] stream-driver XIP replacement active"
ctx_msg:
    .asciiz "CTX="
phase_init_msg:
    .asciiz "[EARLYDIAG] phase=init"
phase_afterspin_msg:
    .asciiz "[EARLYDIAG] phase=afterspin"
msg_a0002200:
    .asciiz "A0002200="
msg_a0002220:
    .asciiz "A0002220="
msg_a0002228:
    .asciiz "A0002228="
msg_a0002274:
    .asciiz "A0002274="
msg_aa000c30:
    .asciiz "AA000C30="
msg_aa000c34:
    .asciiz "AA000C34="
msg_aa000c38:
    .asciiz "AA000C38="
msg_aa000c48:
    .asciiz "AA000C48="
msg_aa000c4c:
    .asciiz "AA000C4C="
msg_af000080:
    .asciiz "AF000080="
msg_af000100:
    .asciiz "AF000100="
hex_digits:
    .ascii  "0123456789ABCDEF"
