/*
 * Bare-metal BE-300 timer/interrupt reproducer.
 *
 * Build modes:
 *   REPRO_TIMER=0 REPRO_IRQ_PROBE=0: ordering control
 *   REPRO_TIMER=1 REPRO_IRQ_PROBE=0: ordering with RTCL1
 *   REPRO_TIMER=1 REPRO_IRQ_PROBE=1: interrupt probe only
 *
 * Guest-visible observations:
 *   STATUS.IE, MSYSINT1, RTCINTREG, SYSINT1, Cause.IP2, vector reached
 *
 * No emulator hooks are used. The binary runs via ./be300 --kernel.
 */

#ifndef REPRO_TIMER
#define REPRO_TIMER 0
#endif

#ifndef REPRO_IRQ_PROBE
#define REPRO_IRQ_PROBE 0
#endif

#if REPRO_IRQ_PROBE
#undef REPRO_TIMER
#define REPRO_TIMER 1
#endif

#define UART_THR_ADDR           0xAA008680
#define UART_LSR_ADDR           0xAA008694
#define UART_LSR_THRE           0x20

#define ICU_SYSINT1_ADDR        0xAF000080
#define ICU_MSYSINT1_ADDR       0xAF00008C
#define ICU_MASK_RTCL1_DOC      0x0004
#define ICU_MASK_TIMER_CPU      ICU_MASK_RTCL1_DOC
#define ICU_SYSINT1_TIMER_SEEN  ICU_MASK_RTCL1_DOC

#define RTC_RTCL1L_ADDR         0xAF000110
#define RTC_RTCINT_ADDR         0xAF00013E
#define RTCINT_RTCLONG1         0x0002
#define RTCL1_DIVISOR           0x0020

#define STATUS_IE_BIT           0x0001
#define STATUS_EXL_BIT          0x0002
#define STATUS_ERL_BIT          0x0004
#define STATUS_ENABLE_ALL       0x3000ff01
#define STATUS_DISABLE_ALL      0x30000000
#define CAUSE_IP2_BIT           0x0400

#define MAGIC0                  0x13579BDF
#define MAGIC1                  0x2468ACE0
#define MAGIC2                  0x0BADCAFE

#define RESULT_NONE             0
#define RESULT_EARLY            1
#define RESULT_READY            2
#define RESULT_BADIRQ           3
#define RESULT_SPURIOUS         4

#define OBS_STATUS_IE           0x0001
#define OBS_MSYSINT1            0x0002
#define OBS_MSYSINT1_DOC        0x0100
#define OBS_RTCINTREG           0x0004
#define OBS_SYSINT1             0x0008
#define OBS_CAUSE_IP2           0x0010
#define OBS_VECTOR              0x0020
#define OBS_STATUS_EXL          0x0040
#define OBS_STATUS_ERL          0x0080

#define CONTROL_DELAY_ITERS     2000000
#define OBSERVE_WINDOW_ITERS    10000

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

    jal     clear_runtime_state
    nop

    lui     $a0, %hi(msg_boot)
    addiu   $a0, $a0, %lo(msg_boot)
    jal     puts
    nop

#if REPRO_IRQ_PROBE
    lui     $a0, %hi(msg_mode_irq_probe)
    addiu   $a0, $a0, %lo(msg_mode_irq_probe)
#elif REPRO_TIMER
    lui     $a0, %hi(msg_mode_timer)
    addiu   $a0, $a0, %lo(msg_mode_timer)
#else
    lui     $a0, %hi(msg_mode_control)
    addiu   $a0, $a0, %lo(msg_mode_control)
#endif
    jal     puts
    nop

#if REPRO_IRQ_PROBE
    jal     arm_rtcl1_timer
    nop
    lui     $a0, %hi(msg_irq_armed)
    addiu   $a0, $a0, %lo(msg_irq_armed)
    jal     puts
    nop
    jal     enable_irqs
    nop
    jal     observe_window
    nop
    jal     disable_irqs
    nop
    jal     sample_interrupt_observation
    nop
    jal     print_observations
    nop
    jal     print_irq_probe_verdict
    nop
#elif REPRO_TIMER
    jal     arm_rtcl1_timer
    nop
    jal     publish_object
    nop
    lui     $a0, %hi(msg_publish)
    addiu   $a0, $a0, %lo(msg_publish)
    jal     puts
    nop
    jal     enable_irqs
    nop
    jal     observe_window
    nop
    jal     disable_irqs
    nop
    jal     sample_interrupt_observation
    nop
    lui     $t0, %hi(irq_count)
    lw      $t0, %lo(irq_count)($t0)
    beqz    $t0, 1f
    nop
    jal     consumer_check
    nop
1:
    jal     finish_object
    nop
    lui     $a0, %hi(msg_done)
    addiu   $a0, $a0, %lo(msg_done)
    jal     puts
    nop
    jal     sample_interrupt_observation
    nop
    lui     $t0, %hi(irq_count)
    lw      $t0, %lo(irq_count)($t0)
    beqz    $t0, 1f
    nop
    lui     $a0, %hi(msg_irq_seen)
    addiu   $a0, $a0, %lo(msg_irq_seen)
    jal     puts
    nop
1:
    jal     consumer_check
    nop
    jal     print_observations
    nop
    jal     print_timer_verdict
    nop
#else
    jal     publish_object
    nop
    lui     $a0, %hi(msg_publish)
    addiu   $a0, $a0, %lo(msg_publish)
    jal     puts
    nop
    jal     busy_window_control
    nop
    jal     finish_object
    nop
    lui     $a0, %hi(msg_done)
    addiu   $a0, $a0, %lo(msg_done)
    jal     puts
    nop
    jal     consumer_check
    nop
    jal     print_control_verdict
    nop
#endif

halt_forever:
    b       halt_forever
    nop

handle_irq:
    lui     $k0, %hi(irq_count)
    lw      $k1, %lo(irq_count)($k0)
    addiu   $k1, $k1, 1
    sw      $k1, %lo(irq_count)($k0)

    lui     $k0, %hi(obs_flags)
    lw      $k1, %lo(obs_flags)($k0)
    ori     $k1, $k1, OBS_VECTOR
    sw      $k1, %lo(obs_flags)($k0)

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
    li      $k1, RTCINT_RTCLONG1
    lui     $k0, %hi(RTC_RTCINT_ADDR)
    sh      $k1, %lo(RTC_RTCINT_ADDR)($k0)
    lui     $k0, %hi(ICU_MSYSINT1_ADDR)
    sh      $zero, %lo(ICU_MSYSINT1_ADDR)($k0)

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
    li      $t0, STATUS_DISABLE_ALL
    mtc0    $t0, $12
    nop
    nop
    nop
    jr      $ra
    nop

enable_irqs:
    li      $t0, STATUS_ENABLE_ALL
    mtc0    $t0, $12
    nop
    nop
    nop
    jr      $ra
    nop

clear_runtime_state:
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
    lui     $t0, %hi(obs_flags)
    sw      $zero, %lo(obs_flags)($t0)
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

    li      $t1, ICU_MASK_TIMER_CPU
    lui     $t0, %hi(ICU_MSYSINT1_ADDR)
    sh      $t1, %lo(ICU_MSYSINT1_ADDR)($t0)
    jr      $ra
    nop

sample_interrupt_observation:
    li      $t7, 0

    mfc0    $t0, $12             /* Status */
    nop
    andi    $t1, $t0, STATUS_IE_BIT
    beqz    $t1, 1f
    nop
    ori     $t7, $t7, OBS_STATUS_IE
1:
    andi    $t1, $t0, STATUS_EXL_BIT
    beqz    $t1, 2f
    nop
    ori     $t7, $t7, OBS_STATUS_EXL
2:
    andi    $t1, $t0, STATUS_ERL_BIT
    beqz    $t1, 3f
    nop
    ori     $t7, $t7, OBS_STATUS_ERL
3:
    lui     $t0, %hi(ICU_MSYSINT1_ADDR)
    lhu     $t0, %lo(ICU_MSYSINT1_ADDR)($t0)
    andi    $t1, $t0, ICU_MASK_TIMER_CPU
    beqz    $t1, 4f
    nop
    ori     $t7, $t7, OBS_MSYSINT1
4:
    lui     $t0, %hi(ICU_MSYSINT1_ADDR)
    lhu     $t0, %lo(ICU_MSYSINT1_ADDR)($t0)
    andi    $t1, $t0, ICU_MASK_RTCL1_DOC
    beqz    $t1, 5f
    nop
    ori     $t7, $t7, OBS_MSYSINT1_DOC
5:
    lui     $t0, %hi(RTC_RTCINT_ADDR)
    lhu     $t0, %lo(RTC_RTCINT_ADDR)($t0)
    andi    $t1, $t0, RTCINT_RTCLONG1
    beqz    $t1, 6f
    nop
    ori     $t7, $t7, OBS_RTCINTREG
6:
    lui     $t0, %hi(ICU_SYSINT1_ADDR)
    lhu     $t0, %lo(ICU_SYSINT1_ADDR)($t0)
    andi    $t1, $t0, ICU_SYSINT1_TIMER_SEEN
    beqz    $t1, 7f
    nop
    ori     $t7, $t7, OBS_SYSINT1
7:
    mfc0    $t0, $13             /* Cause */
    nop
    andi    $t1, $t0, CAUSE_IP2_BIT
    beqz    $t1, 7f
    nop
    ori     $t7, $t7, OBS_CAUSE_IP2
7:
    lui     $t0, %hi(obs_flags)
    lw      $t1, %lo(obs_flags)($t0)
    or      $t1, $t1, $t7
    sw      $t1, %lo(obs_flags)($t0)
    jr      $ra
    nop

observe_window:
    addiu   $sp, $sp, -8
    sw      $ra, 4($sp)
    sw      $s0, 0($sp)
    li      $s0, OBSERVE_WINDOW_ITERS
1:
    jal     sample_interrupt_observation
    nop
    lui     $t1, %hi(obs_flags)
    lw      $t1, %lo(obs_flags)($t1)
    andi    $t2, $t1, OBS_VECTOR
    bnez    $t2, 2f
    nop
    addiu   $s0, $s0, -1
    bnez    $s0, 1b
    nop
2:
    lw      $s0, 0($sp)
    lw      $ra, 4($sp)
    addiu   $sp, $sp, 8
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

busy_window_control:
    li      $t0, CONTROL_DELAY_ITERS
1:
    addiu   $t0, $t0, -1
    bnez    $t0, 1b
    nop
    jr      $ra
    nop

print_observations:
    addiu   $sp, $sp, -8
    sw      $ra, 4($sp)
    lui     $t0, %hi(obs_flags)
    lw      $t1, %lo(obs_flags)($t0)
    li      $t2, OBS_STATUS_IE
    and     $t3, $t1, $t2
    beqz    $t3, 1f
    nop
    lui     $a0, %hi(msg_seen_status_ie)
    addiu   $a0, $a0, %lo(msg_seen_status_ie)
    jal     puts
    nop
1:
    lui     $t0, %hi(obs_flags)
    lw      $t1, %lo(obs_flags)($t0)
    li      $t2, OBS_MSYSINT1
    and     $t3, $t1, $t2
    beqz    $t3, 2f
    nop
    lui     $a0, %hi(msg_seen_msysint1)
    addiu   $a0, $a0, %lo(msg_seen_msysint1)
    jal     puts
    nop
2:
    lui     $t0, %hi(obs_flags)
    lw      $t1, %lo(obs_flags)($t0)
    li      $t2, OBS_MSYSINT1_DOC
    and     $t3, $t1, $t2
    beqz    $t3, 3f
    nop
    lui     $a0, %hi(msg_seen_msysint1_doc)
    addiu   $a0, $a0, %lo(msg_seen_msysint1_doc)
    jal     puts
    nop
3:
    lui     $t0, %hi(obs_flags)
    lw      $t1, %lo(obs_flags)($t0)
    li      $t2, OBS_RTCINTREG
    and     $t3, $t1, $t2
    beqz    $t3, 4f
    nop
    lui     $a0, %hi(msg_seen_rtcint)
    addiu   $a0, $a0, %lo(msg_seen_rtcint)
    jal     puts
    nop
4:
    lui     $t0, %hi(obs_flags)
    lw      $t1, %lo(obs_flags)($t0)
    li      $t2, OBS_SYSINT1
    and     $t3, $t1, $t2
    beqz    $t3, 5f
    nop
    lui     $a0, %hi(msg_seen_sysint1)
    addiu   $a0, $a0, %lo(msg_seen_sysint1)
    jal     puts
    nop
5:
    lui     $t0, %hi(obs_flags)
    lw      $t1, %lo(obs_flags)($t0)
    li      $t2, OBS_CAUSE_IP2
    and     $t3, $t1, $t2
    beqz    $t3, 6f
    nop
    lui     $a0, %hi(msg_seen_cause_ip2)
    addiu   $a0, $a0, %lo(msg_seen_cause_ip2)
    jal     puts
    nop
6:
    lui     $t0, %hi(obs_flags)
    lw      $t1, %lo(obs_flags)($t0)
    li      $t2, OBS_VECTOR
    and     $t3, $t1, $t2
    beqz    $t3, 7f
    nop
    lui     $a0, %hi(msg_seen_vector)
    addiu   $a0, $a0, %lo(msg_seen_vector)
    jal     puts
    nop
7:
    lui     $t0, %hi(obs_flags)
    lw      $t1, %lo(obs_flags)($t0)
    li      $t2, OBS_STATUS_EXL
    and     $t3, $t1, $t2
    beqz    $t3, 8f
    nop
    lui     $a0, %hi(msg_seen_status_exl)
    addiu   $a0, $a0, %lo(msg_seen_status_exl)
    jal     puts
    nop
8:
    lui     $t0, %hi(obs_flags)
    lw      $t1, %lo(obs_flags)($t0)
    li      $t2, OBS_STATUS_ERL
    and     $t3, $t1, $t2
    beqz    $t3, 9f
    nop
    lui     $a0, %hi(msg_seen_status_erl)
    addiu   $a0, $a0, %lo(msg_seen_status_erl)
    jal     puts
    nop
9:
    lw      $ra, 4($sp)
    addiu   $sp, $sp, 8
    jr      $ra
    nop

print_control_verdict:
    addiu   $sp, $sp, -8
    sw      $ra, 4($sp)
    lui     $t0, %hi(consumer_result)
    lw      $t1, %lo(consumer_result)($t0)
    li      $t2, RESULT_READY
    beq     $t1, $t2, 1f
    nop
    li      $t2, RESULT_SPURIOUS
    beq     $t1, $t2, 2f
    nop
    lui     $a0, %hi(msg_verdict_control_bad)
    addiu   $a0, $a0, %lo(msg_verdict_control_bad)
    b       3f
    nop
1:
    lui     $a0, %hi(msg_verdict_control_ready)
    addiu   $a0, $a0, %lo(msg_verdict_control_ready)
    b       3f
    nop
2:
    lui     $a0, %hi(msg_verdict_spurious)
    addiu   $a0, $a0, %lo(msg_verdict_spurious)
3:
    jal     puts
    nop
    lw      $ra, 4($sp)
    addiu   $sp, $sp, 8
    jr      $ra
    nop

print_irq_probe_verdict:
    addiu   $sp, $sp, -8
    sw      $ra, 4($sp)
    lui     $t0, %hi(consumer_result)
    lw      $t1, %lo(consumer_result)($t0)
    li      $t2, RESULT_SPURIOUS
    beq     $t1, $t2, 1f
    nop

    lui     $t0, %hi(obs_flags)
    lw      $t1, %lo(obs_flags)($t0)
    li      $t2, OBS_VECTOR
    and     $t3, $t1, $t2
    bnez    $t3, 2f
    nop
    li      $t2, OBS_CAUSE_IP2
    and     $t3, $t1, $t2
    bnez    $t3, 3f
    nop
    li      $t2, OBS_RTCINTREG | OBS_SYSINT1
    and     $t3, $t1, $t2
    bnez    $t3, 4f
    nop
    lui     $a0, %hi(msg_verdict_irq_no_source)
    addiu   $a0, $a0, %lo(msg_verdict_irq_no_source)
    b       5f
    nop
1:
    lui     $a0, %hi(msg_verdict_spurious)
    addiu   $a0, $a0, %lo(msg_verdict_spurious)
    b       5f
    nop
2:
    lui     $a0, %hi(msg_verdict_irq_vector)
    addiu   $a0, $a0, %lo(msg_verdict_irq_vector)
    b       5f
    nop
3:
    lui     $a0, %hi(msg_verdict_irq_cpu_pending)
    addiu   $a0, $a0, %lo(msg_verdict_irq_cpu_pending)
    b       5f
    nop
4:
    lui     $a0, %hi(msg_verdict_irq_source_only)
    addiu   $a0, $a0, %lo(msg_verdict_irq_source_only)
5:
    jal     puts
    nop
    lw      $ra, 4($sp)
    addiu   $sp, $sp, 8
    jr      $ra
    nop

print_timer_verdict:
    addiu   $sp, $sp, -8
    sw      $ra, 4($sp)
    lui     $t0, %hi(consumer_result)
    lw      $t1, %lo(consumer_result)($t0)
    li      $t2, RESULT_SPURIOUS
    beq     $t1, $t2, 1f
    nop

    lui     $t0, %hi(obs_flags)
    lw      $t4, %lo(obs_flags)($t0)
    li      $t2, OBS_VECTOR
    and     $t3, $t4, $t2
    beqz    $t3, 5f
    nop

    li      $t2, RESULT_EARLY
    beq     $t1, $t2, 2f
    nop
    li      $t2, RESULT_READY
    beq     $t1, $t2, 3f
    nop
    b       4f
    nop
1:
    lui     $a0, %hi(msg_verdict_spurious)
    addiu   $a0, $a0, %lo(msg_verdict_spurious)
    b       8f
    nop
2:
    lui     $a0, %hi(msg_verdict_timer_vector_early)
    addiu   $a0, $a0, %lo(msg_verdict_timer_vector_early)
    b       8f
    nop
3:
    lui     $a0, %hi(msg_verdict_timer_vector_ready)
    addiu   $a0, $a0, %lo(msg_verdict_timer_vector_ready)
    b       8f
    nop
4:
    lui     $a0, %hi(msg_verdict_timer_vector_noresult)
    addiu   $a0, $a0, %lo(msg_verdict_timer_vector_noresult)
    b       8f
    nop
5:
    li      $t2, OBS_CAUSE_IP2
    and     $t3, $t4, $t2
    bnez    $t3, 6f
    nop
    li      $t2, OBS_RTCINTREG | OBS_SYSINT1
    and     $t3, $t4, $t2
    bnez    $t3, 7f
    nop
    lui     $a0, %hi(msg_verdict_timer_no_source)
    addiu   $a0, $a0, %lo(msg_verdict_timer_no_source)
    b       8f
    nop
6:
    lui     $a0, %hi(msg_verdict_timer_cpu_pending)
    addiu   $a0, $a0, %lo(msg_verdict_timer_cpu_pending)
    b       8f
    nop
7:
    lui     $a0, %hi(msg_verdict_timer_source_only)
    addiu   $a0, $a0, %lo(msg_verdict_timer_source_only)
8:
    jal     puts
    nop
    lw      $ra, 4($sp)
    addiu   $sp, $sp, 8
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
#if REPRO_IRQ_PROBE
msg_mode_irq_probe:
    .asciiz "MODE=IRQ_PROBE\n"
#elif REPRO_TIMER
msg_mode_timer:
    .asciiz "MODE=TIMER\n"
#else
msg_mode_control:
    .asciiz "MODE=CONTROL\n"
#endif
msg_irq_armed:
    .asciiz "IRQ_ARMED\n"
msg_publish:
    .asciiz "PUBLISH\n"
msg_done:
    .asciiz "PRODUCER_DONE\n"
msg_irq_seen:
    .asciiz "IRQ_SEEN\n"
msg_seen_status_ie:
    .asciiz "SEEN=STATUS_IE\n"
msg_seen_msysint1:
    .asciiz "SEEN=MSYSINT1\n"
msg_seen_msysint1_doc:
    .asciiz "SEEN=MSYSINT1_DOCBIT\n"
msg_seen_rtcint:
    .asciiz "SEEN=RTCINTREG\n"
msg_seen_sysint1:
    .asciiz "SEEN=SYSINT1\n"
msg_seen_cause_ip2:
    .asciiz "SEEN=CAUSE_IP2\n"
msg_seen_vector:
    .asciiz "SEEN=VECTOR\n"
msg_seen_status_exl:
    .asciiz "SEEN=STATUS_EXL\n"
msg_seen_status_erl:
    .asciiz "SEEN=STATUS_ERL\n"
msg_verdict_control_ready:
    .asciiz "VERDICT=CONTROL_READY\n"
msg_verdict_control_bad:
    .asciiz "VERDICT=CONTROL_BAD\n"
msg_verdict_irq_no_source:
    .asciiz "VERDICT=IRQ_NO_SOURCE\n"
msg_verdict_irq_source_only:
    .asciiz "VERDICT=IRQ_SOURCE_ONLY\n"
msg_verdict_irq_cpu_pending:
    .asciiz "VERDICT=IRQ_CPU_PENDING_NO_VECTOR\n"
msg_verdict_irq_vector:
    .asciiz "VERDICT=IRQ_VECTOR\n"
msg_verdict_timer_no_source:
    .asciiz "VERDICT=TIMER_NO_SOURCE\n"
msg_verdict_timer_source_only:
    .asciiz "VERDICT=TIMER_SOURCE_ONLY\n"
msg_verdict_timer_cpu_pending:
    .asciiz "VERDICT=TIMER_CPU_PENDING_NO_VECTOR\n"
msg_verdict_timer_vector_badirq:
    .asciiz "VERDICT=TIMER_VECTOR_BADIRQ\n"
msg_verdict_timer_vector_noresult:
    .asciiz "VERDICT=TIMER_VECTOR_NORESULT\n"
msg_verdict_timer_vector_early:
    .asciiz "VERDICT=TIMER_VECTOR_EARLY\n"
msg_verdict_timer_vector_ready:
    .asciiz "VERDICT=TIMER_VECTOR_READY\n"
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
obs_flags:
    .space 4
