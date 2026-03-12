/*
 * $Id: fileops.h,v 1.3 2004/08/28 21:29:33 be300 Exp $
 *
 * fileops.h - filesystem independent operations
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

//----------------------------------------------------------------------
// Defines and includes
//
#define	SEEK_SET 0
#define	SEEK_CUR 1
#define	SEEK_END 2
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002

//----------------------------------------------------------------------
// File operations
//
int open (LPCWSTR, int);
int close (int);
int read (int, void *, unsigned int);
int lseek (int, long, int);
int write (int, void *, unsigned int);

#ifdef __cplusplus
}
#endif
