// Project: Linux4.BE
// Copyright © 2002 Filip Onkelinx
//
// http://www.linux4.BE/
// filip@linux4.BE
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
//
//
// $Author: onkelinx $
// $Date: 2002/02/25 15:30:41 $
// $Revision: 1.1.1.1 $
//
// $Header: /home/cvsroot/ce/bevideo/entry.S,v 1.1.1.1 2002/02/25 15:30:41 onkelinx Exp $
//
// -------------------------------------------------------------------------------

#include <asm/asm.h>
#include <asm/regdef.h>
#include <asm/stackframe.h>
#include <asm/mipsregs.h>

	NESTED(kernel_entry, 16, sp)

	CLI

	la	sp, __init_end
	jal	start
	nop

	END(kernel_entry)


