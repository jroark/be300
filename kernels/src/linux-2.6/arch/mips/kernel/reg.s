	.file	1 "arch/mips/kernel/reg.c"
	.data
	.align	0
$Lsb_data:
	.section .bss
	.align	0
$Lsb_bss:
	.sdata
	.align	0
$Lsb_sdata:
	.section .sbss
	.align	0
$Lsb_sbss:
	.text
	.version	"01.01"
gcc2_compiled.:
 #APP
		.macro	_ssnop					
		sll	$0, $2, 1				
		.endm						
								
		#						
		# There is a hazard but we do not care		
		#						
		.macro	irq_enable_hazard			
		.endm						
								
		.macro	irq_disable_hazard			
		_ssnop; _ssnop; _ssnop				
		.endm
	.macro	local_irq_enable
	.set	push
	.set	reorder
	.set	noat
	mfc0	$1,$12
	ori	$1,0x1f
	xori	$1,0x1e
	mtc0	$1,$12
	irq_enable_hazard
	.set	pop
	.endm
	.macro	local_irq_disable
	.set	push
	.set	noat
	mfc0	$1,$12
	ori	$1,1
	xori	$1,1
	.set	noreorder
	mtc0	$1,$12
	irq_disable_hazard
	.set	pop
	.endm
	.macro	local_save_flags flags
	.set	push
	.set	reorder
	mfc0	\flags, $12
	.set	pop
	.endm
	.macro	local_irq_save result
	.set	push
	.set	reorder
	.set	noat
	mfc0	\result, $12
	ori	$1, \result, 1
	xori	$1, 1
	.set	noreorder
	mtc0	$1, $12
	irq_disable_hazard
	.set	pop
	.endm
	.macro	local_irq_restore flags
	.set	noreorder
	.set	noat
	mfc0	$1, $12
	andi	\flags, 1
	ori	$1, 1
	xori	$1, 1
	or	\flags, $1
	mtc0	\flags, $12
	irq_disable_hazard
	.set	at
	.set	reorder
	.endm
 #NO_APP
	.align	2
	.align	3
	.globl	output_ptreg_defines
	.type	output_ptreg_defines,@function
	.set	nomips16
	.ent	output_ptreg_defines
output_ptreg_defines:
	.frame	$sp,0,$31		# vars= 0, regs= 0/0, args= 0, extra= 0
	.mask	0x00000000,0
	.fmask	0x00000000,0
 #APP
	
@@@/* MIPS pt_regs indices. */
	
@@@#define EF_R0     6
	
@@@#define EF_R1     7
	
@@@#define EF_R2     8
	
@@@#define EF_R3     9
	
@@@#define EF_R4     10
	
@@@#define EF_R5     11
	
@@@#define EF_R6     12
	
@@@#define EF_R7     13
	
@@@#define EF_R8     14
	
@@@#define EF_R9     15
	
@@@#define EF_R10    16
	
@@@#define EF_R11    17
	
@@@#define EF_R12    18
	
@@@#define EF_R13    19
	
@@@#define EF_R14    20
	
@@@#define EF_R15    21
	
@@@#define EF_R16    22
	
@@@#define EF_R17    23
	
@@@#define EF_R18    24
	
@@@#define EF_R19    25
	
@@@#define EF_R20    26
	
@@@#define EF_R21    27
	
@@@#define EF_R22    28
	
@@@#define EF_R23    29
	
@@@#define EF_R24    30
	
@@@#define EF_R25    31
	
@@@#define EF_R26    32
	
@@@#define EF_R27    33
	
@@@#define EF_R28    34
	
@@@#define EF_R29    35
	
@@@#define EF_R30    36
	
@@@#define EF_R31    37
	
@@@
	
@@@#define EF_LO     39
	
@@@#define EF_HI     40
	
@@@
	
@@@#define EF_EPC    43
	
@@@#define EF_BVADDR 41
	
@@@#define EF_STATUS 38
	
@@@#define EF_CAUSE  42
	
@@@
	
@@@#define EF_SIZE   176
	
@@@
 #NO_APP
	j	$31
	.end	output_ptreg_defines
	.size	output_ptreg_defines,.-output_ptreg_defines

