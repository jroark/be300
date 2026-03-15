/*
 * $Id: debug.h,v 1.3 2004/08/28 21:29:33 be300 Exp $
 *
 * debug.h - utilities for debugging
 *
 * Copyright (C) 1999 Steve Hill <sjhill@plutonium.net>
 * Copyright (C) 2002 Filip Onkelinx <Filip@Linux4.BE>
 *
 * Modified for the Linux4.BE project by Filip Onkelinx <Filip@Linux4.BE>
 * $Author: be300 $
 * $Date: 2004/08/28 21:29:33 $
 * $Revision: 1.3 $
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

#define DBG_LOADLCE

#define	MSG_ERROR (MB_OK | MB_ICONSTOP)        
#define MSG_INFO  (MB_OK | MB_ICONINFORMATION)

//----------------------------------------------------------------------
// Print debug string back to host environment.
//
int debug_printf(LPCTSTR lpszFmt, ...);

int debug_open();
//----------------------------------------------------------------------
// Print debug string to a message box on the target.
//
int msg_printf(UINT type, LPCTSTR pszCaption, LPCTSTR lpszFmt, ...);

#ifdef __cplusplus
}
#endif
