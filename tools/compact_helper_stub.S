.set noreorder

    .text
    .align 2

ret_one:
    jr      $ra
    addiu   $v0, $zero, 1

ret_zero:
    jr      $ra
    addu    $v0, $zero, $zero

no_op:
    jr      $ra
    nop
