/*
 * $Id: fileops.c,v 1.3 2004/08/28 21:29:33 be300 Exp $
 *
 * fileops.c - filesystem independent operations
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
#include "debug.h"
#include "fileops.h"

static int winfs_open (LPCWSTR filename, int mode);
static int ext2fs_open (LPCWSTR filename, int mode);
static int winfs_close (int fd);
static int ext2fs_close (int fd);
static int winfs_read (int fd, void *buf, unsigned int count);
static int ext2fs_read (int fd, void *buf, unsigned int count);
static int winfs_lseek (int fd, long offset, int whence);
static int ext2fs_lseek (int fd, long offset, int whence);
static int winfs_write (int fd, void *buf, unsigned int count);
static int ext2fs_write (int fd, void *buf, unsigned int count);

//----------------------------------------------------------------------
// Define file system operations in a file system independent way.
//
struct fs_ops {
	int	(*open)		(LPCWSTR filename, int mode);
	int	(*close)	(int fd);
	int	(*read)		(int fd, void *buf, unsigned int count);
	int	(*seek)		(int fd, long offset, int whence);
	int (*write)	(int fd, void *buf, unsigned int count);
};

static struct fs_ops filesystem[] = {
	{winfs_open, winfs_close, winfs_read, winfs_lseek, winfs_write},
	{ext2fs_open, ext2fs_close, ext2fs_read, ext2fs_lseek, ext2fs_write} };

static struct fs_ops *fs;

//----------------------------------------------------------------------
// Open file - interface function to opening a file
//
int open (LPCWSTR filename, int mode)
{
	// Check to see which filesystem to use.
	if (*filename != '/')
		fs = &filesystem[0];
	else
		fs = &filesystem[1];

	// Call the function.
	return fs->open (filename, mode);
}

//----------------------------------------------------------------------
// Close file - interface function to closing a file
//
int	close (int fd)
{
	return fs->close (fd);
}

//----------------------------------------------------------------------
// Read file - interface function to reading a file
//
int	read (int fd, void *buf, unsigned int count)
{
	return fs->read (fd, buf, count);
}

//----------------------------------------------------------------------
// Seek file - interface function to seeking in a file
//
int	lseek (int fd, long offset, int whence)
{
	return fs->seek (fd, offset, whence);
}

//----------------------------------------------------------------------
// Seek file - interface function to seeking in a file
//
int write (int fd, void *buf, unsigned int count)
{
	return fs->write (fd, buf, count);
}

//----------------------------------------------------------------------
// Open file - open a file on a WindowsCE filesystem
//
static int winfs_open (LPCWSTR filename, int mode)
{
	HANDLE hfile;
	DWORD dwAccess, dwShareMode, dwCreationDisposition, dwFlags;

	// Defaults for file modes.
	dwShareMode = 0;
	dwCreationDisposition = OPEN_EXISTING;
	dwFlags = FILE_ATTRIBUTE_NORMAL;

	// Parse the mode string.
	if (mode == O_RDONLY)
		dwAccess = GENERIC_READ;
	else if (mode == O_RDWR)
		dwAccess = GENERIC_READ | GENERIC_WRITE;
	else if (mode == O_WRONLY)
	{
		dwCreationDisposition = CREATE_ALWAYS;
		dwAccess = GENERIC_WRITE;
	}
	else
		return -1;

	// Perform the file operation
	hfile = CreateFile (filename, dwAccess, dwShareMode,
		NULL, dwCreationDisposition, dwFlags, NULL);

	// See if everything is okay.
	if (hfile == INVALID_HANDLE_VALUE)
	{
		return -1;
	}

	// We can do this because 'void *' and 'int' are
	// the same size.
	return (int) hfile;
}

//----------------------------------------------------------------------
// Open file - open a file on an ext2 filesystem
//
static int ext2fs_open (LPCWSTR filename, int mode)
{
	return -1;
}

//----------------------------------------------------------------------
// Close file - close a file on a WindowsCE filesystem
//
static int winfs_close (int fd)
{
	// Always set this at the beginning of each file operation.
	SetLastError (0);

	// Close the file.
	if (CloseHandle ((HANDLE) fd) == TRUE)
		return 0;
	else
	{
		//dwError = GetLastError ();
		msg_printf(MSG_ERROR, _T("Error"), TEXT("Close failed with code %i."), GetLastError());
		return -1;
	}
}

//----------------------------------------------------------------------
// Close file - close a file on an ext2 filesystem
//
static int ext2fs_close (int fd)
{
	return -1;
}

//----------------------------------------------------------------------
// Read file - read a file on a WindowsCE filesystem
//
static int winfs_read (int fd, void *buf, unsigned int count)
{
	BOOL success;
	DWORD bytesRead;

	// Always set this at the beginning of each file operation.
	SetLastError (0);

	// Read that funky file.
	success = ReadFile ((HANDLE) fd, buf, (DWORD) count, &bytesRead, NULL);

	// Return proper value.
	if (success == TRUE)
		return (int) bytesRead;
	else
	{
		msg_printf(MSG_ERROR, _T("Error"), _T("Read failed with code %i."), GetLastError ());
		return -1;
	}
}

//----------------------------------------------------------------------
// Read file - read a file on an ext2 filesystem
//
static int ext2fs_read (int fd, void *buf, unsigned int count)
{
	return 0;
}

//----------------------------------------------------------------------
// Seek in file - seek in a file on a WindowsCE filesystem
//
static int winfs_lseek (int fd, long offset, int whence)
{
	DWORD dwPtr;

	// Always set this at the beginning of each file operation.
	SetLastError (0);

	// Assumes all offsets less than 4GB.
	dwPtr = SetFilePointer ((HANDLE) fd, offset, NULL, whence);

	// Test for failure.
	if (dwPtr == 0xFFFFFFFF)
	{
		msg_printf(MSG_ERROR, _T("Error"), _T("Seek failed with code %i"), GetLastError ());
		return -1;
	}

	// Return happy.
	return (int) dwPtr;
}

//----------------------------------------------------------------------
// Seek in file - seek in a file on an ext2 filesystem
//
static int ext2fs_lseek (int fd, long offset, int whence)
{
	return 0;
}

//----------------------------------------------------------------------
// Write a file - write a file on a WindowsCE filesystem
//
static int winfs_write (int fd, void *buf, unsigned int count)
{
	DWORD written;

	WriteFile((HANDLE) fd, buf, count, &written, NULL);
	return written;
}

//----------------------------------------------------------------------
// Write a file - write a file on an ext2 filesystem
//
static int ext2fs_write (int fd, void *buf, unsigned int count)
{
	return 0;
}
