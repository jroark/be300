/*
 * $Id: config.cpp,v 1.3 2004/08/28 21:29:33 be300 Exp $
 *
 * config.cpp
 *
 * Copyright (C) 1999 Steve Hill <sjhill@plutonium.net>
 * Copyright (C) 1999 Bradley D. LaRonde <brad@ltc.com>
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

#include "windows.h"
#include "bdl/misc.h"
#include "bdl/vector.h"
#include "fileops.h"
#include "config.h"

const TCHAR szImageEntry[] =    _T("image");
const TCHAR szLabelEntry[] =   _T("label");
const TCHAR szAppendEntry[] =   _T("append");
const TCHAR szTimeoutEntry[] =  _T("timeout");
const TCHAR szURLEntry[] =  _T("URL");

BootConfig::BootConfig() : m_nTimeout(5)
{
}

BOOL BootConfig::Interpret(LPCTSTR pszEntry, LPCTSTR pszValue)
{
	// "timeout" is a global value
	if (_tcsicmp(pszEntry, szTimeoutEntry) == 0)
	{
		m_nTimeout = _ttoi(pszValue);
		return TRUE;
	}

	// "image" starts a new image section
	if (_tcsicmp(pszEntry, szImageEntry) == 0)
	{
		// add this image to the boot list
		m_ImageSections.Add(new ImageSection(pszValue));
		return TRUE;
	}

	// "append" applies to the current image section
	if (_tcsicmp(pszEntry, szAppendEntry) == 0)
	{
		// get the current section index
		int nCurrentSection = m_ImageSections.Size() - 1;

		// is there a current section?
		if ( nCurrentSection >= 0 )
		{
			// set the current section's append
			ImageSection* pSection = m_ImageSections.At(nCurrentSection);
			if ( pSection )
				pSection->SetAppend(pszValue);
		}
		return TRUE;
	}

	// "label" applies to the current image section
	if (_tcsicmp(pszEntry, szLabelEntry) == 0)
	{
		// get the current section index
		int nCurrentSection = m_ImageSections.Size() - 1;

		// is there a current section?
		if ( nCurrentSection >= 0 )
		{
			// set the current section's append
			ImageSection* pSection = m_ImageSections.At(nCurrentSection);
			if ( pSection )
				pSection->SetLabel(pszValue);
		}
		return TRUE;
	}

	// "URL" applies to the current image section
	if (_tcsicmp(pszEntry, szURLEntry) == 0)
	{
		// get the current section index
		int nCurrentSection = m_ImageSections.Size() - 1;

		// is there a current section?
		if ( nCurrentSection >= 0 )
		{
			// set the current section's append
			ImageSection* pSection = m_ImageSections.At(nCurrentSection);
			if ( pSection )
				pSection->SetURL(pszValue);
		}
		return TRUE;
	}

	// not interpreted
	return FALSE;
}

BOOL BootConfig::Load(LPCTSTR pszFile)
{
	// open the specified config file
	int fd = open(pszFile, O_RDONLY);
	if(fd == -1)
		return FALSE;

	// read and interpret Entry=Value lines
	// config file is not Unicode
	BOOL bInComment = FALSE;
	char szEntry[30];
	char *pszEntry = szEntry;
	while (read(fd, pszEntry, 1) != 0)
	{
		// is it a comment?
		if (*pszEntry == '#')
		{
			bInComment = TRUE;
			continue;
		}
		
		// is it the end of the line?
		if ((*pszEntry == '\r') || (*pszEntry == '\n') || (*pszEntry == '\f'))
		{
			// not in comment anymore
			bInComment = FALSE;
			pszEntry = szEntry;
			continue;
		}

		// If we are still in comment, iterate.
		if ( bInComment )
			continue;

		// See if we found a header and then get the data.
		if (*pszEntry == '=')
		{
			// we have the entry name
			*pszEntry = '\0';

			// reset the Entry buffer
			pszEntry = szEntry;

			// read the entry value
			char szValue[256];
			char *p = szValue;
			while (read(fd, p, 1) != 0)
			{
				if ((*p == '\r') || (*p == '\n') || (*p == '\f'))
					break;
				p++;
			}
			*p = '\0';

			// convert entry to unicode
			TCHAR szwEntry[256];
			mbstowcs(szwEntry, szEntry, MAX_TCHARS(szwEntry));

			// convert value to unicode
			TCHAR szwValue[256];
			mbstowcs(szwValue, szValue, MAX_TCHARS(szwValue));

			// interpret the Entry-Value pair
			Interpret(szwEntry, szwValue);
		}
		else
			// spot for next header char
			pszEntry++;
	}

	close(fd);

	return TRUE;
}

BOOL BootConfig::Save(LPCTSTR pszFile)
{
	int fd = open(pszFile, O_WRONLY);
	if( fd == -1)
		return FALSE;

	// Write out comment line.
	char szBuff[512];
	strcpy(szBuff, "# Comments look like this.\r\n");
	write(fd, szBuff, strlen(szBuff));

	// use swprintf for these since CE 2.0 doesn't have sprintf
	WCHAR szwBuff[512];
	wsprintf(szwBuff, _T("%s=%d\r\n"), szTimeoutEntry, m_nTimeout);
	wcstombs(szBuff, szwBuff, MAX_CHARS(szBuff));
	write(fd, szBuff, strlen(szBuff));

	int i;
	for ( i = 0; i < m_ImageSections.Size(); i++ )
	{
		wsprintf(szwBuff, _T("%s=%s\r\n"),
			szImageEntry, m_ImageSections.At(i)->GetImage());
		wcstombs(szBuff, szwBuff, MAX_CHARS(szBuff));
		write(fd, szBuff, strlen(szBuff));

		wsprintf(szwBuff, _T("%s=%s\r\n"),
			szImageEntry, m_ImageSections.At(i)->GetLabel());
		wcstombs(szBuff, szwBuff, MAX_CHARS(szBuff));
		write(fd, szBuff, strlen(szBuff));

		wsprintf(szwBuff, _T("%s=%s\r\n"),
			szImageEntry, m_ImageSections.At(i)->GetURL());
		wcstombs(szBuff, szwBuff, MAX_CHARS(szBuff));
		write(fd, szBuff, strlen(szBuff));

		if (lstrlen(m_ImageSections.At(i)->GetAppend()) > 0)
		{
			wsprintf(szwBuff, _T("%s=%s\r\n"),
				szAppendEntry, m_ImageSections.At(i)->GetAppend());
			wcstombs(szBuff, szwBuff, MAX_CHARS(szBuff));
			write(fd, szBuff, strlen(szBuff));
		}
	}

	// Close the file.
	close(fd);

	// Return happy.
	return TRUE;
}
