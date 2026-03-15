/*
 * $Id: mips-pr31700.c,v 1.3 2004/08/28 21:29:33 be300 Exp $
 *
 * mips-pr31700.c - final phase boot code for PR31700
 *
 * Copyright (C) 1999 Steve Hill <sjhill@plutonium.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include <windows.h>
#include "../pbsdboot/pbsdboot.h"

void asm_code_begin_pr31700();
void asm_code_end_pr31700();

int startprog_philips_pr31700(caddr_t map)
{
  int i;
  unsigned char *mem;
  unsigned long JumpInstruction, PhysMem;
  unsigned char *codep = (unsigned char *) asm_code_begin_pr31700;
  int code_len = (unsigned char *) asm_code_end_pr31700 - codep;

  // Allocate physical memory.
  if ((mem = (unsigned char *)vmem_alloc()) == NULL)
  {
    debug_printf(TEXT ("Cannot allocate final page.\n"));
 	msg_printf(MSG_ERROR, _T("Error"), TEXT("Cannot allocate root page.\n"));
    return -1;
  }

  // Copy startup program code.
  for (i = 0; i < code_len; i++)
    mem[i] = *codep++;
  
  // Set map address.
  *(unsigned short*)&mem[0] = (unsigned short)(((long)map) >> 16);
  *(unsigned short*)&mem[4] = (unsigned short)map;

  // Construct start instruction.
  PhysMem = (unsigned long) vtophysaddr(mem);
  JumpInstruction = (0x08000000 | ((PhysMem >> 2) & 0x03ffffff));

  // Map the interrupt vector.
  mem = (unsigned char *) VirtualAlloc(0, 0x400, MEM_RESERVE, PAGE_NOACCESS);
//  VirtualCopy ((LPVOID) mem, (LPVOID) (platformInfo.iAddress >> 8),
  VirtualCopy ((LPVOID) mem, (LPVOID) (0x80000000 >> 8),
	  0x400, PAGE_READWRITE | PAGE_NOCACHE | PAGE_PHYSICAL);

  // Go!!!
  *(unsigned long *) &mem[0x0] = JumpInstruction;

  // We should not get here.
  return 0;
}

void PR31700AssemblyCodeHolder ()
{
/*
 * void
 * startprog(register struct map_s *map)
 * {
 *   register unsigned char *addr;
 *   register unsigned char *p;
 *   register int i;
 * 
 *   addr = map->base;
 *   i = 0;
 *   while (p = map->leaf[i / map->leafsize][i % map->leafsize]) {
 *     register unsigned char *pe = p + map->pagesize;
 *     while (p < pe) {
 *       *addr++ = *p++;
 *     }
 *     i++;
 *   }
 * }
 *
 *  register assignment:
 *    struct map_s *map		a0
 *    unsigned char *addr	a1
 *    unsigned char *p		a2
 *    unsigned char *pe		a3
 *    int i			t0
 *
 * struct map_s {
 *   caddr_t entry;	+0
 *   caddr_t base;	+4
 *   int pagesize;	+8
 *   int leafsize;	+12
 *   int nleaves;	+16
 *   caddr_t arg0;	+20

 *   caddr_t arg1;  +24

 *   caddr_t arg2;	+28

 *   caddr_t arg3;	+32

 *   caddr_t *leaf[32];	+36
 *
 */
__asm(
"	.set noreorder;	"
"	.globl asm_code_begin_pr31700;"
"asm_code_begin_pr31700:"
"	lui	a0, 0x0000;	"
"	ori	a0, 0x0000;	"

/* addr = map->base;	*/
"lw	a1, 4(a0);"

/*   i = 0;		*/
"ori	t0, zero, 0;"

" loop_start:"

/*   while (p = map->leaf[i / map->leafsize][i % map->leafsize]) { */
/*   t1 = map->leafsize */
"lw	t1, 12(a0);"

/*   lo = i / map->leafsize, hi = i % map->leafsize */
"addu	t3, zero, t0;"
"div	t3, t1;"
/*   t2 = map->leaf */
"addiu	t2, a0, 36;"
/*   t3 = i / map->leafsize */
"nop;"
"mflo	t3;"
/*   t2 = map->leaf[i / map->leafsize] */
"sll	t3, t3, 2;"
"addu	t2, t2, t3;"
"lw	t2, 0(t2);"
/*   t3 = i % map->leafsize */
"mfhi	t3;"

/*   p = map->leaf[i / map->leafsize][i % map->leafsize] */
"sll	t3, t3, 2;"
"addu	t2, t2, t3;"
"lw	a2, 0(t2);"

/*		if (p == NULL) {	*/
/*			break;			*/
/*		}					*/
"beq	a2, zero, loop_end;"
"nop;"

/*     register unsigned char *pe = p + map->pagesize; */
"lw	t1, 8(a0);"
"add	a3, a2, t1;"

/*		while (p < pe) {		*/
"loop_start2:"
"sltu        t1, a2, a3;"
"beq         zero,t1,loop_end2;"
"nop;"

/*			*addr++ = *p++;	*/
"lw	t1, 0(a2);"
"sw	t1, 0(a1);"
"addi	a2, a2, 4;"
"addi	a1, a1, 4;"

/*		}	*/
"beq	zero, zero, loop_start2;"
"nop;"

/*     i++;	*/
"loop_end2:"
"addi	t0, t0, 1;"
"beq	zero, zero, loop_start;"
"nop;"

" loop_end:"
);


/*
 *  flush instruction cache
 */
__asm(
"	li	t0, 0x44000000;"
"	addu	t1, t0, 1024*128;"
"	subu	t1, t1, 128;"
"1:"
"	cache	0, 0(t0);"
"	cache	0, 16(t0);"
"	cache	0, 32(t0);"
"	cache	0, 48(t0);"
"	cache	0, 64(t0);"
"	cache	0, 80(t0);"
"	cache	0, 96(t0);"
"	cache	0, 112(t0);"
"	bne	t0, t1, 1b;"
"	addu	t0, t0, 128;"
);

/*
 *  flush data cache
 */
__asm(
"	li	t0, 0x44000000;"
"	addu	t1, t0, 1024*128;"
"	subu	t1, t1, 128;"
"1:"
"	cache   1, 0(t0);"
"	cache   1, 16(t0);"
"	cache   1, 32(t0);"
"	cache   1, 48(t0);"
"	cache   1, 64(t0);"
"	cache   1, 80(t0);"
"	cache   1, 96(t0);"
"	cache   1, 112(t0);"
"	bne     t0, t1, 1b;"
"	addu    t0, t0, 128;"
);

__asm(
"	lw	t0, 0(a0);"		/* entry addr */
"	lw	a1, 24(a0);"	/* arg1 */
"	lw	a2, 28(a0);"	/* arg2 */
"	lw	a3, 32(a0);"	/* arg3 */
"	lw	a0, 20(a0);"	/* arg0 */
"	jr	t0;"
"	nop;"

"	.globl asm_code_end_pr31700;"
"asm_code_end_pr31700: nop;"
"	.set reorder;	"
);
}
