#include "asm_stub.h"

	.text
	.set noreorder

NESTED(kernel_entry, 16, $ra)
	/* Setup stack pointer */
	la	$sp, _end
	addiu	$sp, $sp, 0x4000
	
	/* Call start */
	jal	start
	nop

	/* Loop forever */
1:	b	1b
	nop
	
END(kernel_entry)
