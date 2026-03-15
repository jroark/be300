/*
 * $Id: configurator.cpp,v 1.3 2004/08/28 21:29:33 be300 Exp $
 *
 * configurator.c
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

#include <windows.h>
#include <commctrl.h>
#include "bdl/misc.h"
#include "bdl/system.h"
#include "bdl/vector.h"
#include "bdl/dialog.h"
#include "resource.h"
#include "../lib/config.h"
#include "../lib/fileops.h"
#include "configurator.h"


int Configurator::Main()
{
	TCHAR szMsg[512];

	InitCommonControls();

	// figure the absolute config file name
	TCHAR szConfigFile[MAX_PATH];
	System::AbsolutePath(szConfigFile, _T("cyacecfg.txt"));

	// load the boot configuration file
	BootConfig config;
	BOOL bResult = config.Load(szConfigFile);
	if ( !bResult ) {
		_stprintf(szMsg, _T("Error %d loading boot configuration file %s."),
			GetLastError(), szConfigFile);
		System::ErrorMessageBox(szMsg);
		return 1;
	}

	// no image sections?
	if ( config.GetImageSections()->Size() == 0 ) {
		_stprintf(szMsg, _T("Error - no image sections in configuration file %s."),
			szConfigFile);
		System::ErrorMessageBox(szMsg);
		return 1;
	}

	// get the default image section
	ImageSection* pDefaultSection = config.GetImageSections()->At(0);
	if ( pDefaultSection == 0 ) {
		_stprintf(szMsg, _T("Error - no image sections defined in configuration file %s."),
			szConfigFile);
		System::ErrorMessageBox(szMsg);
		return 1;
	}

	// figure the default boot command
	TCHAR szBootCommand[1024];
	_tcscpy(szBootCommand, pDefaultSection->GetLabel());

	// split off the section label
	TCHAR szLabel[256];
	TCHAR szParameters[2048];
	_stscanf(szBootCommand, _T("%s %[^\0]"), szLabel, szParameters);

	// find the specified section
	ImageSection* pSection = 0;
	int i;
	for ( i = 0; i < config.GetImageSections()->Size(); i++ )
	{
		if ( _tcsicmp(szLabel, config.GetImageSections()->At(i)->GetLabel()) == 0 )
		{
			pSection = config.GetImageSections()->At(i);
			break;
		}
	}

	// didn't find the section?
	if ( pSection == 0 ) {
		_stprintf(szMsg, _T("Error - can't find section %s in configuration file %s."),
			szLabel, szConfigFile);
		System::ErrorMessageBox(szMsg);
		return 1;
	}

	return 0;
}
