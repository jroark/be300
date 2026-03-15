// system.h

// Written by Bradley D. LaRonde, brad@ltc.com.
// Copyright (C) 1999 Bradley D. LaRonde.

// A Java-like System class, which can somewhat be described
// as a glorified container for global data.

// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

class System;

class SysVar
{
private:
	HINSTANCE m_hInstance;
	HINSTANCE m_hPrevInstance;
	LPWSTR m_lpCmdLine;
	int m_nCmdShow;
	TCHAR m_szApplicationName[MAX_PATH];
	TCHAR m_szModuleFileName[MAX_PATH];
	TCHAR m_szStartupDirectory[MAX_PATH];
	HWND m_hwndMain;

	SysVar()
	{
	}

	void Set(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine,
		int nCmdShow, LPCTSTR szApplicationName)
	{
		m_hInstance = hInstance;
		m_hPrevInstance = hPrevInstance;
		m_lpCmdLine = lpCmdLine;
		m_nCmdShow = nCmdShow;
		_tcscpy(m_szApplicationName, szApplicationName);

		GetModuleFileName(hInstance, m_szModuleFileName, MAX_TCHARS(m_szModuleFileName));

		// figure the startup directory
		// used as a default path for config and kernel files
		_tcscpy(m_szStartupDirectory, m_szModuleFileName);
		TCHAR* pszLastBackslash = _tcsrchr(m_szStartupDirectory, _T('\\'));
		if ( pszLastBackslash )
			*(pszLastBackslash + 1) = '\0';
	}

	friend System;
};

class System
{
private:
	static SysVar _sv;

public:
	System(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine,
		int nCmdShow, LPCTSTR szApplicationName)
	{
		_sv.Set(hInstance, hPrevInstance, lpCmdLine, nCmdShow, szApplicationName);
	}

	static HINSTANCE GetModuleInstance()
	{
		return _sv.m_hInstance;
	}

	static LPCTSTR GetModuleFileName()
	{
		return _sv.m_szModuleFileName;
	}

	static LPCTSTR GetApplicationName()
	{
		return _sv.m_szApplicationName;
	}

	static LPCTSTR GetStartupDirectory()
	{
		return _sv.m_szStartupDirectory;
	}

	static int MessageBox(LPCTSTR msg, LPCTSTR caption, UINT type)
	{
		return ::MessageBox(_sv.m_hwndMain, msg, _sv.m_szApplicationName, type);
	}

	static int InfoMessageBox(LPCTSTR msg)
	{
		return MessageBox(msg, _sv.m_szApplicationName,
			MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
	}

	static int ErrorMessageBox(LPCTSTR msg)
	{
		return MessageBox(msg, _sv.m_szApplicationName,
			MB_OK | MB_ICONSTOP | MB_SETFOREGROUND);
	}

	static int YesNoMessageBox(LPCTSTR msg)
	{
		return MessageBox(msg, _sv.m_szApplicationName,
			MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
	}

	static LPTSTR AbsolutePath(LPTSTR dest, LPCTSTR src)
	{
		// is it a relative path?
		if ( *src != _T('\\') )
		{
			_tcscpy(dest, System::GetStartupDirectory());
			_tcscat(dest, src);
		}
		else
			_tcscpy(dest, src);

		return dest;
	}

};
