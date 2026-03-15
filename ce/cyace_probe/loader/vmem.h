/*
 * $Id: vmem.h,v 1.2 2002/05/02 09:43:34 filip Exp $
 *
 * vmem.h - virtual memory utilities
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

#ifdef __cplusplus
extern "C" {
#endif

struct map_s {
  caddr_t entry;
  caddr_t base;
  int pagesize;
  int leafsize;
  int nleaves;
  caddr_t arg0;
  caddr_t arg1;
  caddr_t arg2;
  caddr_t arg3;
  caddr_t *leaf[32];
};

// Allocates memory from locally created heap.
caddr_t VmemAlloc();

// Initializes and allocates memory for kernel and arguments.
int VmemInit(caddr_t start, caddr_t end, unsigned int phys_start);

// Frees all allocated memory if anything fails.
void VmemFree();

// Constructs jump instruction and then executes the kernel.
caddr_t VmemPrepareExec(caddr_t entry, int argc, char *argv[]);

// Gets the virtual address that maps to a physical address.
caddr_t VmemGet(caddr_t phys_addr, int *length);

// Converts a virtual address to a physical address.
caddr_t VirtToPhysAddr(caddr_t page);

#ifdef __cplusplus
}
#endif
