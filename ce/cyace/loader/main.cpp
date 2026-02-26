/*
 * $Id: main.cpp,v 1.4 2004/08/28 21:29:33 be300 Exp $
 *
 * main.cpp
 *
 * Copyright (C) 1999 Bradley D. LaRonde <brad@ltc.com>
 * Copyright (C) 2002 Filip Onkelinx <Filip@Linux4.BE>
 *
 * Modified for the Linux4.BE project by Filip Onkelinx <Filip@Linux4.BE>
 * $Author: be300 $
 * $Date: 2004/08/28 21:29:33 $
 * $Revision: 1.4 $
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

#define _TIME_T

#include <windows.h>
#include "../pbsdboot/pbsdboot.h"
#include "bdl/misc.h"
#include "bdl/system.h"
#include "bdl/dialog.h"
#include "loader.h"

// for app and ui
DialogMap g_DialogMap;
SysVar System::_sv;

// for pbsdboot
preference_s pref;
unsigned int phys_start;
startprog_t startprog;
CheckCancel_t CheckCancel;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
					LPWSTR lpCmdLine, int nCmdShow)
				{
	// the one and only System object
	new System(hInstance, hPrevInstance, lpCmdLine, nCmdShow,
		_T("CyaCE ELF Program Loader"));

	return (new Loader)->Main();
}
