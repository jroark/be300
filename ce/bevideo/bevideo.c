// Project: Linux4.BE
// Copyright � 2002 Filip Onkelinx
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
// $Header: /home/cvsroot/ce/bevideo/bevideo.c,v 1.1.1.1 2002/02/25 15:30:41 onkelinx Exp $
//
// -------------------------------------------------------------------------------

void start(void)
{
unsigned long *addr; 		
int x,y;

while(1)
{
   addr=(void *)0xaa200000;
		for (y=0;y<105;y++)
		{
			for (x=0;x<80;x++)
			{
				*(unsigned long *)(addr+256*y+x)=0xF800;  //RED
			}
		}
		for (y=105;y<210;y++)
		{
			for (x=80;x<160;x++)
			{
				*(unsigned long *)(addr+256*y+x)= 0x07e0;  //GREEN
			}
		}
		for (y=210;y<320;y++)
		{
			for (x=160;x<240;x++)
			{
				*(unsigned long *)(addr+256*y+x)= 0x001F;  //BLUE
			}
		}

	}

}


static int initialized = 0;

