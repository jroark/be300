/*
 * $Id: debug.c,v 1.4 2004/08/28 21:29:33 be300 Exp $
 *  
 * debug.c - utilities for debugging
 *
 * Copyright (C) 1999 Steve Hill <sjhill@plutonium.net>
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

#include <windows.h>
#include "debug.h"
#include "winbase.h"

static HANDLE hSerial = INVALID_HANDLE_VALUE;

int debug_open()
{
	DCB dcb;

	if (hSerial != INVALID_HANDLE_VALUE)
		return 0;

	const TCHAR szSerialPort[] = _T("COM1:");
	hSerial = CreateFile(szSerialPort, GENERIC_READ | GENERIC_WRITE,
		0, 0, OPEN_EXISTING, 0, 0);
	if ( hSerial != INVALID_HANDLE_VALUE )
	{
		GetCommState(hSerial,&dcb);
		dcb.BaudRate=CBR_115200;
		SetCommState(hSerial,&dcb);
		debug_printf(_T("Serial port opened.\n"));
	}
	
	return(0);
}

void debug_close()
{
	if (hSerial != INVALID_HANDLE_VALUE)
	{
		CloseHandle(hSerial);
		hSerial = INVALID_HANDLE_VALUE;
	}
}

//----------------------------------------------------------------------
// DebugHostPrint - print debug string back to host environment
//
int debug_printf(LPCTSTR lpszFmt, ...)
{
	DWORD dwActual;
	int count;
	va_list ap;
	wchar_t buffer[1024];
	char szMessage[1024];

	// Get the list of arguments.
	va_start (ap, lpszFmt);
	count = wvsprintf (buffer, lpszFmt, ap);
	va_end (ap);

	// If there is at least one text string, output it.
	if (count > 0 && hSerial != INVALID_HANDLE_VALUE){
		wcstombs(szMessage, buffer, 512);
		strcat(szMessage, "\r\n");
		WriteFile(hSerial, szMessage, strlen(szMessage), &dwActual, 0);
		FlushFileBuffers(hSerial);
	}
	Sleep(1000);
	return count;
}


//----------------------------------------------------------------------
// DebugTargPrint - print debug string to a message box on the target
//
#ifdef DBG_LOADLCE
int msg_printf(UINT type, LPCTSTR pszCaption, LPCTSTR lpszFmt, ...)
{
	va_list ap;
	TCHAR buffer[1024];

	// Get the list of arguments.
	va_start (ap, lpszFmt);
	wvsprintf (buffer, lpszFmt, ap);
	va_end (ap);

	// If there is at least one text string, output it.
	return MessageBox(NULL, buffer, pszCaption, type);
}
#else
int msg_printf(UINT type, LPCTSTR pszCaption, LPCTSTR lpszFmt, ...)
{
	return 0;
}
#endif
