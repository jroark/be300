.set noreorder

    .text
    .align 2

    .org 0x045c
close_impl:
    jr      $ra
    addiu   $v0, $zero, 1

    .org 0x05b8
entry_impl:
    jr      $ra
    addiu   $v0, $zero, 1

    .org 0x0c38
powerdown_impl:
    jr      $ra
    nop

    .org 0x0c4c
powerup_impl:
    jr      $ra
    nop

    .org 0x0c54
zero_impl:
    jr      $ra
    addu    $v0, $zero, $zero

    .org 0x0f44
iocontrol_impl:
    jr      $ra
    addiu   $v0, $zero, 1

    .org 0x10c8
open_impl:
    jr      $ra
    addiu   $v0, $zero, 1

    .org 0x10d0
deinit_impl:
    jr      $ra
    addiu   $v0, $zero, 1

    .org 0x1104
init_impl:
    jr      $ra
    addiu   $v0, $zero, 1
