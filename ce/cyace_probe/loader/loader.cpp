/*
 * $Id: loader.cpp,v 1.5 2004/08/28 21:29:33 be300 Exp $
 *
 * loader.c - load and execute an elf executable
 *
 * Copyright (C) 1999 Steve Hill <sjhill@plutonium.net>
 * Copyright (C) 1999 Bradley D. LaRonde <brad@ltc.com>
 * Copyright (C) 2002 Filip Onkelinx <Filip@Linux4.BE>
 *
 * Modified for the Linux4.BE project by Filip Onkelinx <Filip@Linux4.BE>
 * $Author: be300 $
 * $Date: 2004/08/28 21:29:33 $
 * $Revision: 1.5 $
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
#include <stdlib.h>
#include <winsock.h>
#include <commctrl.h>
#include "../pbsdboot/pbsdboot.h"
#include "bdl/misc.h"
#include "bdl/system.h"
#include "bdl/vector.h"
#include "bdl/dialog.h"
#include "resource.h"
#include "bdl/progress.h"
#include "../lib/config.h"
#include "loader.h"

#include <malloc.h>

extern "C" startprog_nec_vr41xx(caddr_t map);
extern "C" startprog_philips_pr31700(caddr_t map);


class PbsdbootProgressCallbackAdapter : public ProgressCallbackAdapter
{
private:
	static unsigned int m_nCalls;
	static unsigned int m_nSkip;

public:
	PbsdbootProgressCallbackAdapter(ProgressDialog* pProgressDialog)
		: ProgressCallbackAdapter(pProgressDialog)
	{
	}

	static void SetSkip(unsigned int nSkip)
	{
		m_nSkip = nSkip;
	}

	static BOOL CheckCancel(int nProgress)
	{
		// only update every so many calls
		// this speeds things up a little bit
		if ( PbsdbootProgressCallbackAdapter::m_nCalls++ % m_nSkip == 0 )
			return Update(nProgress);
		else
			return ProgressCallbackAdapter::m_pProgressDialog->ShouldCancel();
	}
};

unsigned int PbsdbootProgressCallbackAdapter::m_nCalls = 0;
unsigned int PbsdbootProgressCallbackAdapter::m_nSkip = 1;

static void ProbeLogBegin(LPCTSTR pszKernel, LPCTSTR pszSource)
{
	debug_printf(_T("[CYACE_PROBE] begin kernel=%s source=%s"),
		pszKernel, pszSource);
}

static void ProbeLogFail(LPCTSTR pszStage, DWORD gle, int result)
{
	debug_printf(_T("[CYACE_PROBE] fail stage=%s gle=%lu result=%d"),
		pszStage, (unsigned long)gle, result);
}

Loader::Processor Loader::GetProcessor()
{
	const unsigned char PRID_NEC_VR41XX = 0x0c;
	const unsigned char PRID_PHILIPS_PR31700 = 0x22;

	SYSTEM_INFO sysinfo;
	GetSystemInfo(&sysinfo);

	switch ( sysinfo.wProcessorArchitecture )
	{
		case PROCESSOR_ARCHITECTURE_MIPS:
			switch ( sysinfo.wProcessorRevision >> 8 )
			{
				case PRID_NEC_VR41XX:
					return NEC_VR41XX;

				case PRID_PHILIPS_PR31700:
					return PHILLIPS_PR31700;

				default:
					return PROCESSOR_UNKNOWN;
			}

		default:
			return PROCESSOR_UNKNOWN;
	}
}

BOOL Loader::GetProcessorSpecificSettings(unsigned int* pps, startprog_t* psp)
{
	switch ( GetProcessor() )
	{
		case NEC_VR41XX:
			*pps = 0x80000000;
			*psp = startprog_nec_vr41xx;
			return TRUE;

		case PHILLIPS_PR31700:
			*pps = 0x44000000;
			*psp = startprog_philips_pr31700;
			return TRUE;

		default:
			return FALSE;
	}
}

void Loader::Load(ImageSection* pSection, LPCTSTR pszParameters, LPCTSTR pszBootURL)
{
	TCHAR msg[512];

	pref.from_network = 0;
	if (*pszBootURL != 0)
	{
		pref.from_network = 1;
		LoadURL(pSection, pszParameters, pszBootURL);
		return;
	}

	// get the processor specific settings
	BOOL bResult = GetProcessorSpecificSettings(&phys_start, &startprog);
	if ( !bResult )
	{
		SYSTEM_INFO sysinfo;
		GetSystemInfo(&sysinfo);
		_stprintf(msg, _T("Unsupported processor revision 0x%.4x"),
			sysinfo.wProcessorRevision);
		System::ErrorMessageBox(msg);
		return;
	}

	// don't load debug_info
	pref.load_debug_info = 0;

	// figure the kernel image file name
	TCHAR szImage[MAX_PATH];
	System::AbsolutePath(szImage, pSection->GetImage());

	// Open the kernel file for reading.
	int fd = open(szImage, O_RDONLY);
	if (fd == -1)
	{
		_stprintf(msg, _T("Error %d opening kernel file %s."),
			GetLastError(), szImage);
		System::ErrorMessageBox(msg);
		return;
	}

	debug_open();
	ProbeLogBegin(pSection->GetLabel(), _T("file"));

	// display the boot progress dialog
	ProgressDialog progress;
	progress.SetCaption(System::GetApplicationName());
	HWND hwndProgress = progress.Create(System::GetModuleInstance(), 0);
	if ( hwndProgress == 0 )
	{
		_stprintf(msg, _T("Error %d creating progress dialog."), GetLastError());
		System::ErrorMessageBox(msg);
		// keep going anyway
	}

	PbsdbootProgressCallbackAdapter pca(&progress);
	CheckCancel = pca.CheckCancel;

	_stprintf(msg, _T("Loading %s %s..."), pSection->GetLabel(), pszParameters);
	progress.Update(0, _T(""), msg);

	// convert append
	char szAppend[2048];
	wcstombs(szAppend, pSection->GetAppend(), MAX_CHARS(szAppend));

	// convert parameters
	char szParameters[2048];
	wcstombs(szParameters, pszParameters, MAX_CHARS(szParameters));

	// merge them
	strcat(szParameters, " ");
	strcat(szParameters, szAppend);

	// figure program name for first arg
	char szProgram[MAX_PATH];
	wcstombs(szProgram, System::GetModuleFileName(), MAX_CHARS(szProgram));

	// hold arguments
	char *argv[256];
	int argc = 0;

	// first arg is the program name
	argv[argc++] = szProgram;

	// parse pszParameters into standard argc/argv
	// only to cat them back together again in prom_init,
	// then split them back apart again in parse_options - lol - bdl
	char *parg = strtok(szParameters, " ");
	while ( parg )
	{
		argv[argc++] = parg;
		parg = strtok(0, " ");
	}

	// last arg is null
	argv[argc] = 0;

	progress.Update(0, _T("Figuring start and end addresses..."));

	// Get the start and end address of the kernel memory.
	caddr_t start, end;
	int nResult = getinfo(fd, &start, &end);
	if ( nResult < 0)
	{
		DWORD gle = GetLastError();
		ProbeLogFail(_T("getinfo"), gle, nResult);
		close(fd);
		debug_close();
		_stprintf(msg, _T("Error %d getting elf file info for %s."),
			gle, szImage);
		System::ErrorMessageBox(msg);
		return;
	}

	progress.Update(0, _T("Initializing memory..."));
	pca.SetSkip(40);

	// Initialize the virtual memory for the kernel pages.
	nResult = vmem_init(start, end);
	if ( nResult < 0) {
		DWORD gle = GetLastError();
		ProbeLogFail(_T("vmem_init"), gle, nResult);
		close(fd);
		debug_close();
		return;
	}

	progress.Update(0, _T("Allocating memory for arguments..."));

	// Allocate memory for kernel arguments.
	caddr_t argbuf = vmem_alloc();
	if ( argbuf == 0 )
	{
		DWORD gle = GetLastError();
		ProbeLogFail(_T("argbuf"), gle, -1);
		_stprintf(msg, _T("Error %d allocating memory for kernel arguments."),
			gle);
		System::ErrorMessageBox(msg);
		close(fd);
		vmem_free();
		debug_close();
		return;
	}

	progress.Update(0, _T("Allocating memory for bootinfo struct..."));

	// Allocate memory for kernel arguments.
	struct bootinfo* bibuf = (struct bootinfo*)vmem_alloc();
	if ( bibuf == 0 )
	{
		DWORD gle = GetLastError();
		ProbeLogFail(_T("bootinfo"), gle, -1);
		_stprintf(msg, _T("Error %d allocating memory for bootinfo struct."),
			gle);
		System::ErrorMessageBox(msg);
		close(fd);
		vmem_free();
		debug_close();
		return;
	}

	progress.Update(0, _T("Copying arguments..."));

	// Copy kernel arguments into newly allocated memory.
	// skip past the argv[] array for the argument storage
	caddr_t p = &argbuf[sizeof(char *)* argc];
	unsigned long argbuf_used = sizeof(char *) * argc;
	DWORD argbuf_limit = getpagesize();
	int i;
	for (i = 0; i < argc; i++)
	{
		int arglen = strlen(argv[i]) + 1;
		if (argbuf_used + (unsigned long)arglen > (unsigned long)argbuf_limit)
		{
			ProbeLogFail(_T("argcopy"), ERROR_BUFFER_OVERFLOW, -1);
			_stprintf(msg, _T("Kernel arguments exceed one page (%lu bytes)."),
				(unsigned long)(argbuf_used + (unsigned long)arglen));
			System::ErrorMessageBox(msg);
			close(fd);
			vmem_free();
			debug_close();
			return;
		}
		((char **) argbuf)[i] = p;
		memcpy (p, argv[i], arglen);
		p += arglen;
		argbuf_used += (unsigned long)arglen;
	}

	progress.Update(0, _T("Loading the kernel file..."));
	pca.SetSkip(3);

	// Load the kernel file into memory.
	nResult = loadfile(fd, &start);
	if ( nResult < 0)
	{
		DWORD gle = GetLastError();
		ProbeLogFail(_T("loadfile"), gle, nResult);
		if ( !progress.ShouldCancel() )
		{
			_stprintf(msg, _T("Error %d loading elf file %s."),
				gle, szImage);
			System::ErrorMessageBox(msg);
		}
		close(fd);
		vmem_free();
		debug_close();
		return;
	}
	close(fd);
	fd = -1;

	progress.Update(0, _T("Preparing and executing kernel..."));

	// Go execute initialization code and boot the kernel.
	// if it works, it won't return
	nResult = vmem_exec(start, argc, (char **)argbuf, bibuf);

	// guess it didn't work
	DWORD gle = GetLastError();
	ProbeLogFail(_T("vmem_exec"), gle, nResult);
	_stprintf(msg, _T("Error %d, result %d executing kernel."),
		gle, nResult);
	System::ErrorMessageBox(msg);
	vmem_free();
	debug_close();
}

void Loader::LoadURL(ImageSection* pSection, LPCTSTR pszParameters, LPCTSTR pszBootURL)
{
	TCHAR msg[512];
	TCHAR szServer[64];
	struct hostent *host;
	struct hostent *allocated_host = 0;
	struct sockaddr_in sin;

	// get the processor specific settings
	BOOL bResult = GetProcessorSpecificSettings(&phys_start, &startprog);
	if ( !bResult )
	{
		SYSTEM_INFO sysinfo;
		GetSystemInfo(&sysinfo);
		_stprintf(msg, _T("Unsupported processor revision 0x%.4x"),
			sysinfo.wProcessorRevision);
		System::ErrorMessageBox(msg);
		return;
	}

	// don't load debug_info
	pref.load_debug_info = 0;

	// convert append
	char szURL[2048];
	char server[64];
	int port = 0;
	wcstombs(szURL, pszBootURL, MAX_CHARS(szURL));

	if (sscanf(szURL, "%[^:]:%d", server, &port) == 0)
	{
		_stprintf(msg, _T("Invalid URL: %s"), pszBootURL);
		System::ErrorMessageBox(msg);
		return;
	}
	mbstowcs(szServer, server, MAX_CHARS(szServer));

	// display the boot progress dialog
	ProgressDialog progress;
	progress.SetCaption(System::GetApplicationName());
	HWND hwndProgress = progress.Create(System::GetModuleInstance(), 0);
	if ( hwndProgress == 0 )
	{
		_stprintf(msg, _T("Error %d creating progress dialog."), GetLastError());
		System::ErrorMessageBox(msg);
		// keep going anyway
	}

	PbsdbootProgressCallbackAdapter pca(&progress);
	CheckCancel = pca.CheckCancel;

	_stprintf(msg, _T("Resolving %s..."), szServer);
	progress.Update(0, _T(""), msg);

	//
	sin.sin_addr.S_un.S_addr = inet_addr(server);
	if (sin.sin_addr.S_un.S_addr == INADDR_NONE) {
		host = gethostbyname(server);
		if (!host) {
			_stprintf(msg, _T("Unable to resolve %s"), szServer);
			System::ErrorMessageBox(msg);
			return;
		}
		memcpy(&sin.sin_addr, host->h_addr, host->h_length);
	} else {
		host = (struct hostent *)malloc(sizeof(struct hostent));
		if (!host) {
			System::ErrorMessageBox(_T("Unable to allocate URL host buffer."));
			return;
		}
		allocated_host = host;
		host->h_addrtype = AF_INET;
	}
	sin.sin_family = host->h_addrtype;
	sin.sin_port = htons(port);

	debug_open();
	ProbeLogBegin(pSection->GetLabel(), _T("url"));

	_stprintf(msg, _T("Loading %s %s..."), pSection->GetLabel(), pszParameters);
	progress.Update(0, _T(""), msg);

	// conver image
	char szImage[512];
	wcstombs(szImage, pSection->GetImage(), MAX_CHARS(szImage));

	// convert append
	char szAppend[2048];
	wcstombs(szAppend, pSection->GetAppend(), MAX_CHARS(szAppend));

	// convert parameters
	char szParameters[2048];
	wcstombs(szParameters, pszParameters, MAX_CHARS(szParameters));

	// merge them
	strcat(szParameters, " ");
	strcat(szParameters, szAppend);

	// figure program name for first arg
	char szProgram[MAX_PATH];
	wcstombs(szProgram, System::GetModuleFileName(), MAX_CHARS(szProgram));

	// hold arguments
	char *argv[256];
	int argc = 0;

	// first arg is the program name
	argv[argc++] = szProgram;

	// parse pszParameters into standard argc/argv
	// only to cat them back together again in prom_init,
	// then split them back apart again in parse_options - lol - bdl
	char *parg = strtok(szParameters, " ");
	while ( parg )
	{
		argv[argc++] = parg;
		parg = strtok(0, " ");
	}

	// last arg is null
	argv[argc] = 0;

	progress.Update(0, _T("Figuring start and end addresses..."));

	// Get the start and end address of the kernel memory.
	caddr_t start, end;
	int nResult = getinfo2(&sin, szURL, szImage, &start, &end);
	if ( nResult < 0)
	{
		DWORD gle = GetLastError();
		ProbeLogFail(_T("getinfo2"), gle, nResult);
		_stprintf(msg, _T("Error %d getting elf file info for %s."),
			gle, pSection->GetLabel());
		System::ErrorMessageBox(msg);
		if (allocated_host)
			free(allocated_host);
		debug_close();
		return;
	}

	progress.Update(0, _T("Initializing memory..."));
	pca.SetSkip(40);

	// Initialize the virtual memory for the kernel pages.
	nResult = vmem_init(start, end);
	if ( nResult < 0) {
		DWORD gle = GetLastError();
		ProbeLogFail(_T("vmem_init"), gle, nResult);
		if (allocated_host)
			free(allocated_host);
		debug_close();
		return;
	}

	progress.Update(0, _T("Allocating memory for arguments..."));

	// Allocate memory for kernel arguments.
	caddr_t argbuf = vmem_alloc();
	if ( argbuf == 0 )
	{
		DWORD gle = GetLastError();
		ProbeLogFail(_T("argbuf"), gle, -1);
		_stprintf(msg, _T("Error %d allocating memory for kernel arguments."),
			gle);
		System::ErrorMessageBox(msg);
		if (allocated_host)
			free(allocated_host);
		vmem_free();
		debug_close();
		return;
	}

	progress.Update(0, _T("Allocating memory for bootinfo struct..."));

	// Allocate memory for kernel arguments.
	struct bootinfo* bibuf = (struct bootinfo*)vmem_alloc();
	if ( bibuf == 0 )
	{
		DWORD gle = GetLastError();
		ProbeLogFail(_T("bootinfo"), gle, -1);
		_stprintf(msg, _T("Error %d allocating memory for bootinfo struct."),
			gle);
		System::ErrorMessageBox(msg);
		if (allocated_host)
			free(allocated_host);
		vmem_free();
		debug_close();
		return;
	}

	progress.Update(0, _T("Copying arguments..."));

	// Copy kernel arguments into newly allocated memory.
	// skip past the argv[] array for the argument storage
	caddr_t p = &argbuf[sizeof(char *)* argc];
	unsigned long argbuf_used = sizeof(char *) * argc;
	DWORD argbuf_limit = getpagesize();
	int i;
	for (i = 0; i < argc; i++)
	{
		int arglen = strlen(argv[i]) + 1;
		if (argbuf_used + (unsigned long)arglen > (unsigned long)argbuf_limit)
		{
			ProbeLogFail(_T("argcopy"), ERROR_BUFFER_OVERFLOW, -1);
			_stprintf(msg, _T("Kernel arguments exceed one page (%lu bytes)."),
				(unsigned long)(argbuf_used + (unsigned long)arglen));
			System::ErrorMessageBox(msg);
			if (allocated_host)
				free(allocated_host);
			vmem_free();
			debug_close();
			return;
		}
		((char **) argbuf)[i] = p;
		memcpy (p, argv[i], arglen);
		p += arglen;
		argbuf_used += (unsigned long)arglen;
	}

	progress.Update(0, _T("Loading the kernel file..."));
	pca.SetSkip(3);

	// Load the kernel file into memory.
	nResult = loadfile2(&sin, szURL, szImage, &start);
	if ( nResult < 0)
	{
		DWORD gle = GetLastError();
		ProbeLogFail(_T("loadfile2"), gle, nResult);
		if ( !progress.ShouldCancel() )
		{
			_stprintf(msg, _T("Error %d loading elf file %s."),
				gle, pSection->GetLabel());
			System::ErrorMessageBox(msg);
		}
		if (allocated_host)
			free(allocated_host);
		vmem_free();
		debug_close();
		return;
	}
	if (allocated_host) {
		free(allocated_host);
		allocated_host = 0;
	}

	progress.Update(0, _T("Preparing and executing kernel..."));

	// Go execute initialization code and boot the kernel.
	// if it works, it won't return
	nResult = vmem_exec(start, argc, (char **)argbuf, bibuf);

	// guess it didn't work
	DWORD gle = GetLastError();
	ProbeLogFail(_T("vmem_exec"), gle, nResult);
	_stprintf(msg, _T("Error %d, result %d executing kernel."),
		gle, nResult);
	System::ErrorMessageBox(msg);
	vmem_free();
	debug_close();
}

class LoadDialog : public ModalDialog
{
private:
	int m_nTimeout;
	int m_nCountdown;
	TCHAR m_szLabels[1024];
	TCHAR m_szCommand[1024];
	TCHAR m_szURL[1024];

	void ShowCountdown()
	{
		TCHAR szMsg[512];
		_stprintf(szMsg,
			_T("This program will automatically execute the load")
			_T(" command in %d seconds. Press Esc to cancel the countdown."),
			m_nCountdown);
		SetItemText(IDC_MESSAGE, szMsg);

		// set the progress position
		SendItemMessage(IDC_PROGRESS, PBM_SETPOS, (m_nCountdown * 100) / m_nTimeout, 0);
	}

	void ShowReady()
	{
		SetItemText(IDC_MESSAGE, _T("Ready for load command."));
		::ShowWindow(GetItem(IDC_PROGRESS), SW_HIDE);
	}

	BOOL OnInitDialog()
	{
		if ( m_nTimeout > 0 )
		{
			m_nCountdown = m_nTimeout;
			ShowCountdown();
			SetTimer(m_hwndDlg, 1, 1000, 0);
		}
		else
			ShowReady();

		SetItemText(IDC_LABELS, m_szLabels);

		SetItemText(IDC_COMMAND, m_szCommand);
		SetItemText(IDC_COMMAND2, m_szURL);
		return TRUE;
	}

	BOOL OnOK()
	{
		// retrieve the boot command
		GetItemText(IDC_COMMAND, m_szCommand, MAX_CHARS(m_szCommand));
		GetItemText(IDC_COMMAND2, m_szURL, MAX_CHARS(m_szURL));

		return ModalDialog::OnOK();
	}

	BOOL OnTimer(int nTimer, TIMERPROC* pTimerproc)
	{
		if ( nTimer == 1 )
		{
			if ( --m_nCountdown == 0 )
			{
				OnOK();
				return TRUE;
			}
			
			ShowCountdown();
			return TRUE;
		}

		return FALSE;
	}

	BOOL OnCancel()
	{
		if ( m_nCountdown > 0 )
		{
			KillTimer(m_hwndDlg, 1);
			m_nCountdown = 0;
			ShowReady();
			return TRUE;
		}

		return ModalDialog::OnCancel();
	}

public:
	LoadDialog() :
		ModalDialog(MAKEINTRESOURCE(IDD_LOAD))
	{
	}

	void SetTimeout(int n)
	{
		m_nTimeout = n;
	}

	void SetLabels(LPCTSTR psz)
	{
		_tcscpy(m_szLabels, psz);
	}
	
	void SetCommand(LPCTSTR psz)
	{
		_tcscpy(m_szCommand, psz);
	}

	LPCTSTR GetCommand()
	{
		return m_szCommand;
	}

	void SetURL(LPCTSTR psz)
	{
		_tcscpy(m_szURL, psz);
	}

	LPCTSTR GetURL()
	{
		return m_szURL;
	}
};

void Loader::GetSystemProcessorInfo(LPTSTR psz)
{
	SYSTEM_INFO sysinfo;
	GetSystemInfo(&sysinfo);
	_stprintf(psz, _T("arch %d, type %d, level 0x%.4x, rev 0x%.4x"),
		sysinfo.wProcessorArchitecture, sysinfo.dwProcessorType,
		sysinfo.wProcessorLevel, sysinfo.wProcessorRevision);
}

BOOL Loader::VerifyProcessorSupport()
{
	Processor p = GetProcessor();
	if ( (p == PROCESSOR_UNKNOWN) || (p == PHILLIPS_PR31700) )
	{
		TCHAR info[256];
		GetSystemProcessorInfo(info);
		TCHAR msg[256];
		_stprintf(msg, _T("Unsupported processor - %s. Continue anyway?"), info);
		int nResult = System::YesNoMessageBox(msg);
		if ( nResult != IDYES )
			return FALSE;
	}

	return TRUE;
}

int Loader::Main()
{
	TCHAR szMsg[MAX_PATH * 2];

	m_nVerboseLevel = 0;

	if ( m_nVerboseLevel > 0 )
	{
		TCHAR msg[256];
		GetSystemProcessorInfo(msg);
		System::InfoMessageBox(msg);
	}

	// let them know up front
	if ( !VerifyProcessorSupport() )
		return 1;

	// progress bar needs this
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
	TCHAR szBootURL[1024];
	_tcscpy(szBootCommand, pDefaultSection->GetLabel());
	_tcscpy(szBootURL, pDefaultSection->GetURL());

	// display the boot dialog?
	if ( config.GetTimeout() != 0 )
	{
		LoadDialog dlg;
		dlg.SetCaption(System::GetApplicationName());
		dlg.SetCommand(pDefaultSection->GetLabel());
		dlg.SetURL(pDefaultSection->GetURL());
		dlg.SetTimeout(config.GetTimeout());

		// make a section list
		TCHAR szLabels[1024];
		_tcscpy(szLabels, _T("Available labels: "));
		int i;
		for ( i = 0; i < config.GetImageSections()->Size(); i++ )
		{
			if ( i > 0 )
				_tcscat(szLabels, _T(", "));
			_tcscat(szLabels, config.GetImageSections()->At(i)->GetLabel());
		}
		dlg.SetLabels(szLabels);

		int nResult = dlg.Do(System::GetModuleInstance(), 0);
		if ( nResult != IDOK )
			return 1;

		// get the specified boot command
		_tcscpy(szBootCommand, dlg.GetCommand());

		// get the specified URL
		_tcscpy(szBootURL, dlg.GetURL());
	}

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

	// boot it
	Load(pSection, szParameters, szBootURL);

	// getting here means that it failed

	return 1;
}
