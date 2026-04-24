.set noreorder

    .text
    .align 2

entry_impl:
    jr      $ra
    addiu   $v0, $zero, 1

close_impl:
    jr      $ra
    addiu   $v0, $zero, 1

powerdown_impl:
    jr      $ra
    nop

powerup_impl:
    jr      $ra
    nop

zero_impl:
    jr      $ra
    addu    $v0, $zero, $zero

iocontrol_impl:
    jr      $ra
    addiu   $v0, $zero, 1

open_impl:
    jr      $ra
    addiu   $v0, $zero, 1

deinit_impl:
    jr      $ra
    addiu   $v0, $zero, 1

init_impl:
    jr      $ra
    addiu   $v0, $zero, 1
