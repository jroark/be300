/*
 * Bare-metal BE-300 timer ordering reproducer.
 *
 * Builds two variants from the same source:
 *   REPRO_TIMER=0: control path, no RTCL1 interrupt
 *   REPRO_TIMER=1: RTCL1 interrupt during the producer window
 *
 * The test publishes an object pointer/state before populating the
 * payload fields. The interrupt handler acts as the consumer. If the
 * handler observes the object before payload completion, it records
 * RESULT_EARLY. This exercises the emulator's existing BE-300 UART and
 * VR41xx/RTCL1 interrupt path without touching emulator code.
 */

#ifndef REPRO_TIMER
#define REPRO_TIMER 0
#endif

#define UART_THR_ADDR       0xAA008680
#define UART_LSR_ADDR       0xAA008694
#define UART_LSR_THRE       0x20

#define ICU_MSYSINT1_ADDR   0xAF00008C
#define ICU_SYSINT1_ADDR    0xAF000088
#define ICU_SRC_RTC         0x0004

#define RTC_RTCL1L_ADDR     0xAF000110
#define RTC_RTCINT_ADDR     0xAF00013E
#define RTCINT_RTCLONG1     0x0002
#define RTCL1_DIVISOR       0x0020

#define MAGIC0              0x13579BDF
#define MAGIC1              0x2468ACE0
#define MAGIC2              0x0BADCAFE

#define RESULT_NONE         0
#define RESULT_EARLY        1
#define RESULT_READY        2
#define RESULT_BADIRQ       3
#define RESULT_SPURIOUS     4

    .set noreorder
    .set mips32

    .section .tlbvec, "ax"
    .globl __tlb_vector
__tlb_vector:
    lui     $k0, %hi(spurious_trap)
    addiu   $k0, $k0, %lo(spurious_trap)
    jr      $k0
    nop

    .section .excvec, "ax"
    .globl __exc_vector
__exc_vector:
    mfc0    $k0, $13             /* Cause */
    nop
    andi    $k1, $k0, 0x007c     /* ExcCode field << 2 */
    bnez    $k1, 1f
    nop
    lui     $k0, %hi(handle_irq)
    addiu   $k0, $k0, %lo(handle_irq)
    jr      $k0
    nop
1:
    lui     $k0, %hi(spurious_trap)
    addiu   $k0, $k0, %lo(spurious_trap)
    jr      $k0
    nop

    .text
    .globl _start
_start:
    lui     $sp, %hi(_stack_top)
    ori     $sp, $sp, %lo(_stack_top)

    jal     zero_bss
    nop

    jal     disable_irqs
    nop

    jal     clear_irq_state
    nop

    lui     $a0, %hi(msg_boot)
    addiu   $a0, $a0, %lo(msg_boot)
    jal     puts
    nop

#if REPRO_TIMER
    lui     $a0, %hi(msg_mode_timer)
    addiu   $a0, $a0, %lo(msg_mode_timer)
#else
    lui     $a0, %hi(msg_mode_control)
    addiu   $a0, $a0, %lo(msg_mode_control)
#endif
    jal     puts
    nop

#if REPRO_TIMER
    jal     arm_rtcl1_timer
    nop
#endif

    jal     publish_object
    nop

    lui     $a0, %hi(msg_publish)
    addiu   $a0, $a0, %lo(msg_publish)
    jal     puts
    nop

#if REPRO_TIMER
    jal     enable_irqs
    nop
#endif

    jal     busy_window
    nop

#if REPRO_TIMER
    jal     disable_irqs
    nop
#endif

    jal     finish_object
    nop

    lui     $a0, %hi(msg_done)
    addiu   $a0, $a0, %lo(msg_done)
    jal     puts
    nop

#if REPRO_TIMER
    lui     $t0, %hi(irq_count)
    lw      $t0, %lo(irq_count)($t0)
    beqz    $t0, 1f
    nop
    lui     $a0, %hi(msg_irq_seen)
    addiu   $a0, $a0, %lo(msg_irq_seen)
    jal     puts
    nop
1:
#endif

    jal     consumer_check
    nop

    jal     print_verdict
    nop

halt_forever:
    b       halt_forever
    nop

handle_irq:
    lui     $k0, %hi(irq_count)
    lw      $k1, %lo(irq_count)($k0)
    addiu   $k1, $k1, 1
    sw      $k1, %lo(irq_count)($k0)

    lui     $k0, %hi(RTC_RTCINT_ADDR)
    lhu     $k0, %lo(RTC_RTCINT_ADDR)($k0)
    andi    $k0, $k0, RTCINT_RTCLONG1
    lui     $k1, %hi(irq_path_ok)
    beqz    $k0, 1f
    nop
    li      $k0, 1
    sw      $k0, %lo(irq_path_ok)($k1)
    b       2f
    nop
1:
    sw      $zero, %lo(irq_path_ok)($k1)
2:
    jal     consumer_check
    nop

    lui     $k0, %hi(ICU_MSYSINT1_ADDR)
    sh      $zero, %lo(ICU_MSYSINT1_ADDR)($k0)
    li      $k1, RTCINT_RTCLONG1
    lui     $k0, %hi(RTC_RTCINT_ADDR)
    sh      $k1, %lo(RTC_RTCINT_ADDR)($k0)

    eret
    nop

spurious_trap:
    lui     $k0, %hi(consumer_result)
    li      $k1, RESULT_SPURIOUS
    sw      $k1, %lo(consumer_result)($k0)
    mfc0    $k0, $14             /* EPC */
    nop
    addiu   $k0, $k0, 4
    mtc0    $k0, $14
    nop
    nop
    nop
    eret
    nop

disable_irqs:
    li      $t0, 0x30000000
    mtc0    $t0, $12             /* Status */
    nop
    nop
    nop
    jr      $ra
    nop

enable_irqs:
    li      $t0, 0x3000ff01
    mtc0    $t0, $12             /* Status: IE + all IM bits */
    nop
    nop
    nop
    jr      $ra
    nop

clear_irq_state:
    lui     $t0, %hi(ICU_MSYSINT1_ADDR)
    sh      $zero, %lo(ICU_MSYSINT1_ADDR)($t0)
    li      $t1, RTCINT_RTCLONG1
    lui     $t0, %hi(RTC_RTCINT_ADDR)
    sh      $t1, %lo(RTC_RTCINT_ADDR)($t0)

    lui     $t0, %hi(irq_count)
    sw      $zero, %lo(irq_count)($t0)
    lui     $t0, %hi(irq_path_ok)
    sw      $zero, %lo(irq_path_ok)($t0)
    lui     $t0, %hi(consumer_result)
    sw      $zero, %lo(consumer_result)($t0)
    jr      $ra
    nop

arm_rtcl1_timer:
    mtc0    $zero, $9            /* Count */
    li      $t0, 0x7fffffff
    mtc0    $t0, $11             /* Compare far away; avoid IP7 wake */
    nop
    nop
    nop

    li      $t1, RTCINT_RTCLONG1
    lui     $t0, %hi(RTC_RTCINT_ADDR)
    sh      $t1, %lo(RTC_RTCINT_ADDR)($t0)

    li      $t1, RTCL1_DIVISOR
    lui     $t0, %hi(RTC_RTCL1L_ADDR)
    sh      $t1, %lo(RTC_RTCL1L_ADDR)($t0)

    li      $t1, ICU_SRC_RTC
    lui     $t0, %hi(ICU_MSYSINT1_ADDR)
    sh      $t1, %lo(ICU_MSYSINT1_ADDR)($t0)
    jr      $ra
    nop

publish_object:
    lui     $t0, %hi(shared_payload)
    addiu   $t0, $t0, %lo(shared_payload)
    lui     $t1, %hi(shared_obj_ptr)
    sw      $t0, %lo(shared_obj_ptr)($t1)
    lui     $t1, %hi(shared_published)
    li      $t0, 1
    sw      $t0, %lo(shared_published)($t1)
    jr      $ra
    nop

finish_object:
    lui     $t0, %hi(shared_payload)
    li      $t1, MAGIC0
    sw      $t1, %lo(shared_payload + 0)($t0)
    li      $t1, MAGIC1
    sw      $t1, %lo(shared_payload + 4)($t0)
    li      $t1, MAGIC2
    sw      $t1, %lo(shared_payload + 8)($t0)
    jr      $ra
    nop

consumer_check:
    lui     $t0, %hi(shared_published)
    lw      $t1, %lo(shared_published)($t0)
    beqz    $t1, 9f
    nop

    lui     $t0, %hi(shared_obj_ptr)
    lw      $t1, %lo(shared_obj_ptr)($t0)
    beqz    $t1, 9f
    nop

    lw      $t2, 0($t1)
    li      $t3, MAGIC0
    bne     $t2, $t3, 8f
    nop
    lw      $t2, 4($t1)
    li      $t3, MAGIC1
    bne     $t2, $t3, 8f
    nop
    lw      $t2, 8($t1)
    li      $t3, MAGIC2
    bne     $t2, $t3, 8f
    nop
    li      $t2, RESULT_READY
    b       7f
    nop
8:
    li      $t2, RESULT_EARLY
7:
    lui     $t0, %hi(consumer_result)
    lw      $t3, %lo(consumer_result)($t0)
    bnez    $t3, 9f
    nop
    sw      $t2, %lo(consumer_result)($t0)
9:
    jr      $ra
    nop

busy_window:
#if REPRO_TIMER
    li      $t0, 10000000
#else
    li      $t0, 2000000
#endif
1:
    addiu   $t0, $t0, -1
    bnez    $t0, 1b
    nop
    jr      $ra
    nop

print_verdict:
    lui     $t0, %hi(consumer_result)
    lw      $t1, %lo(consumer_result)($t0)

#if REPRO_TIMER
    lui     $t0, %hi(irq_count)
    lw      $t2, %lo(irq_count)($t0)
    beqz    $t2, 10f
    nop
    lui     $t0, %hi(irq_path_ok)
    lw      $t2, %lo(irq_path_ok)($t0)
    beqz    $t2, 11f
    nop
    li      $t2, RESULT_EARLY
    beq     $t1, $t2, 12f
    nop
    li      $t2, RESULT_READY
    beq     $t1, $t2, 13f
    nop
    li      $t2, RESULT_SPURIOUS
    beq     $t1, $t2, 14f
    nop
    lui     $a0, %hi(msg_verdict_timer_noresult)
    addiu   $a0, $a0, %lo(msg_verdict_timer_noresult)
    b       15f
    nop
10:
    lui     $t0, %hi(RTC_RTCINT_ADDR)
    lhu     $t2, %lo(RTC_RTCINT_ADDR)($t0)
    andi    $t2, $t2, RTCINT_RTCLONG1
    beqz    $t2, 10f
    nop
    lui     $t0, %hi(ICU_SYSINT1_ADDR)
    lhu     $t2, %lo(ICU_SYSINT1_ADDR)($t0)
    andi    $t2, $t2, 0x0008
    beqz    $t2, 10f
    nop
    lui     $a0, %hi(msg_verdict_timer_pending_novector)
    addiu   $a0, $a0, %lo(msg_verdict_timer_pending_novector)
    b       15f
    nop
10:
    lui     $a0, %hi(msg_verdict_timer_noirq)
    addiu   $a0, $a0, %lo(msg_verdict_timer_noirq)
    b       15f
    nop
11:
    lui     $a0, %hi(msg_verdict_timer_badirq)
    addiu   $a0, $a0, %lo(msg_verdict_timer_badirq)
    b       15f
    nop
12:
    lui     $a0, %hi(msg_verdict_timer_early)
    addiu   $a0, $a0, %lo(msg_verdict_timer_early)
    b       15f
    nop
13:
    lui     $a0, %hi(msg_verdict_timer_ready)
    addiu   $a0, $a0, %lo(msg_verdict_timer_ready)
    b       15f
    nop
14:
    lui     $a0, %hi(msg_verdict_spurious)
    addiu   $a0, $a0, %lo(msg_verdict_spurious)
15:
#else
    li      $t2, RESULT_READY
    beq     $t1, $t2, 20f
    nop
    li      $t2, RESULT_SPURIOUS
    beq     $t1, $t2, 21f
    nop
    lui     $a0, %hi(msg_verdict_control_bad)
    addiu   $a0, $a0, %lo(msg_verdict_control_bad)
    b       22f
    nop
20:
    lui     $a0, %hi(msg_verdict_control_ready)
    addiu   $a0, $a0, %lo(msg_verdict_control_ready)
    b       22f
    nop
21:
    lui     $a0, %hi(msg_verdict_spurious)
    addiu   $a0, $a0, %lo(msg_verdict_spurious)
22:
#endif
    jal     puts
    nop
    jr      $ra
    nop

zero_bss:
    lui     $t0, %hi(__bss_start)
    addiu   $t0, $t0, %lo(__bss_start)
    lui     $t1, %hi(__bss_end)
    addiu   $t1, $t1, %lo(__bss_end)
1:
    sltu    $t2, $t0, $t1
    beqz    $t2, 2f
    nop
    sw      $zero, 0($t0)
    addiu   $t0, $t0, 4
    b       1b
    nop
2:
    jr      $ra
    nop

puts:
    addiu   $sp, $sp, -16
    sw      $ra, 12($sp)
    sw      $a0, 8($sp)
1:
    lw      $a0, 8($sp)
    lbu     $t0, 0($a0)
    beqz    $t0, 2f
    nop
    move    $a0, $t0
    jal     putchar
    nop
    lw      $t1, 8($sp)
    addiu   $t1, $t1, 1
    sw      $t1, 8($sp)
    b       1b
    nop
2:
    lw      $ra, 12($sp)
    addiu   $sp, $sp, 16
    jr      $ra
    nop

putchar:
    lui     $t1, %hi(UART_LSR_ADDR)
1:
    lbu     $t0, %lo(UART_LSR_ADDR)($t1)
    andi    $t0, $t0, UART_LSR_THRE
    beqz    $t0, 1b
    nop
    lui     $t1, %hi(UART_THR_ADDR)
    sb      $a0, %lo(UART_THR_ADDR)($t1)
    jr      $ra
    nop

    .data
msg_boot:
    .asciiz "BOOT\n"
#if REPRO_TIMER
msg_mode_timer:
    .asciiz "MODE=TIMER\n"
#else
msg_mode_control:
    .asciiz "MODE=CONTROL\n"
#endif
msg_publish:
    .asciiz "PUBLISH\n"
msg_done:
    .asciiz "PRODUCER_DONE\n"
msg_irq_seen:
    .asciiz "IRQ_SEEN\n"
msg_verdict_control_ready:
    .asciiz "VERDICT=CONTROL_READY\n"
msg_verdict_control_bad:
    .asciiz "VERDICT=CONTROL_BAD\n"
msg_verdict_timer_ready:
    .asciiz "VERDICT=TIMER_READY\n"
msg_verdict_timer_early:
    .asciiz "VERDICT=TIMER_EARLY\n"
msg_verdict_timer_noirq:
    .asciiz "VERDICT=TIMER_NOIRQ\n"
msg_verdict_timer_pending_novector:
    .asciiz "VERDICT=TIMER_PENDING_NOVECTOR\n"
msg_verdict_timer_badirq:
    .asciiz "VERDICT=TIMER_BADIRQ\n"
msg_verdict_timer_noresult:
    .asciiz "VERDICT=TIMER_NORESULT\n"
msg_verdict_spurious:
    .asciiz "VERDICT=SPURIOUS\n"

    .bss
    .align 4
shared_published:
    .space 4
shared_obj_ptr:
    .space 4
shared_payload:
    .space 12
irq_count:
    .space 4
irq_path_ok:
    .space 4
consumer_result:
    .space 4
