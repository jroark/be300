// dialog.h

// Written by Bradley D. LaRonde, brad@ltc.com.
// Copyright (C) 1999 Bradley D. LaRonde.

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

class Dialog
{
private:
	Dialog() {}

protected:
	LPCTSTR m_lpTemplate;
	HINSTANCE m_hInstance;
	HWND m_hwndCreate;
	HWND m_hwndDlg;
	BOOL m_bSetCaption;
	TCHAR m_szCaption[256];

	virtual BOOL OnInitDialog() { return TRUE; }
	virtual BOOL OnCancel() { return FALSE; }
	virtual BOOL OnOK() { return FALSE; }
	virtual BOOL OnCommand(WPARAM wCommand) { return FALSE; }
	virtual BOOL OnTimer(int nTimer, TIMERPROC* pTimerproc) { return FALSE; }
	virtual BOOL OnDefault(UINT uMsg, WPARAM wParam, LPARAM lParam) { return FALSE; }

	void Refresh()
	{
		// process all messages for this dialog
		MSG msg;
		while ( PeekMessage(&msg, m_hwndDlg, 0, 0, PM_REMOVE) )
			IsDialogMessage(m_hwndDlg, &msg);
	}

public:
	void SetCaption(LPCTSTR pszCaption)
	{
		if ( pszCaption == 0 )
		{
			m_bSetCaption = FALSE;
		}
		else
		{
			m_bSetCaption = TRUE;
			_tcscpy(m_szCaption, pszCaption);
		}
	}

	void ShowWindow(int nCmdShow)
	{
		::ShowWindow(m_hwndDlg, nCmdShow);
	}

	HWND GetItem(int nItem)
	{
		return ::GetDlgItem(m_hwndDlg, nItem);
	}

	BOOL SetItemText(int nItem, LPCTSTR pszText)
	{
		HWND hItem = GetItem(nItem);
		BOOL bResult = SetWindowText(hItem, pszText);
		return bResult;
	}

	BOOL GetItemText(int nItem, LPTSTR pszText, DWORD dwMaxChars)
	{
		return GetWindowText(GetItem(nItem), pszText, dwMaxChars);
	}

	LONG SendItemMessage(int nItem, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		return SendDlgItemMessage(m_hwndDlg, nItem, uMsg, wParam, lParam);
	}

	BOOL UpdateItem(int nItem)
	{
		return UpdateWindow(GetItem(nItem));
	}

	BOOL DialogProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		m_hwndDlg = hwndDlg;

		switch(uMsg)
		{
			case WM_INITDIALOG:
				if ( m_bSetCaption )
				{
					SetWindowText(m_hwndDlg, m_szCaption);
					UpdateWindow(m_hwndDlg);
				}

				return OnInitDialog();

			case WM_COMMAND:
				switch(LOWORD(wParam))
				{
					case IDCANCEL:
						return OnCancel();

					case IDOK:
						return OnOK();

					default:
						return OnCommand(wParam);
				}

			case WM_TIMER:
				return OnTimer(wParam, (TIMERPROC*)lParam);

			default:
				return OnDefault(uMsg, wParam, lParam);
		}
	}

	Dialog(LPCTSTR lpTemplate) :
		m_lpTemplate(lpTemplate), m_bSetCaption(FALSE)
	{
	}
};


class DialogMap;

extern DialogMap g_DialogMap;

class DialogMap
{
private:
	enum
	{
		nHwndDialogMapSizeMax = 10
	};

	int m_nHwndDialogMapSize;
	HWND m_HwndDialogMapHwnd[nHwndDialogMapSizeMax];
	Dialog* m_HwndDialogMapDialog[nHwndDialogMapSizeMax];
	Dialog* m_pNextCreatedDialog;

	int FindIndex(HWND hwnd)
	{
		for(int i = 0; i < m_nHwndDialogMapSize; ++i)
			if(m_HwndDialogMapHwnd[i] == hwnd)
				return i;
		return -1;
	}

	void Add(HWND hwnd, Dialog* pDialog)
	{
		m_HwndDialogMapHwnd[m_nHwndDialogMapSize] = hwnd;
		m_HwndDialogMapDialog[m_nHwndDialogMapSize] = pDialog;
		m_nHwndDialogMapSize++;
	}


public:
	void AddNext(Dialog* pDialog)
	{
		m_pNextCreatedDialog = pDialog;
	}

	Dialog* Find(HWND hwnd)
	{
		if ( m_pNextCreatedDialog != 0 )
		{
			Add(hwnd, m_pNextCreatedDialog);
			m_pNextCreatedDialog = 0;
		}

		Dialog* pDialog = 0;

		int i = FindIndex(hwnd);
		if ( i != -1 )
			pDialog = m_HwndDialogMapDialog[i];

		return pDialog;
	}

	void Remove(Dialog* pDialog)
	{
		// not implemented
	}

	static BOOL CALLBACK DialogProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		Dialog* pDialog = g_DialogMap.Find(hwndDlg);
		if ( pDialog == 0 )
			return FALSE;

		return pDialog->DialogProc(hwndDlg, uMsg, wParam, lParam);
	};
};


class ModelessDialog : public Dialog
{
public:
	ModelessDialog(LPCTSTR lpTemplate) :
	  Dialog(lpTemplate) {};

	HWND Create(HINSTANCE hInstance, HWND hWndParent)
	{
		// save the instance
		m_hInstance = hInstance;

		// tell it this is next created dialog
		g_DialogMap.AddNext(this);

		// create the dialog
		m_hwndCreate = CreateDialog(hInstance, m_lpTemplate, hWndParent, DialogMap::DialogProc);

		return m_hwndCreate;
	}

	void Destroy()
	{
		// remove it from the list
		g_DialogMap.Remove(this);

		DestroyWindow(m_hwndDlg);
	}

	~ModelessDialog()
	{
		Destroy();
	}
};


class ModalDialog : public Dialog
{
protected:
	BOOL EndDialog(int nResult) const
	{
		return ::EndDialog(m_hwndDlg, nResult);
	}

	virtual BOOL OnOK()
	{
		return EndDialog(IDOK);
	}

	virtual BOOL OnCancel()
	{
		return EndDialog(IDCANCEL);
	}


public:
	ModalDialog(LPCTSTR lpTemplate) :
	  Dialog(lpTemplate) {};

	WPARAM Do(HINSTANCE hInstance, HWND hWndParent)
	{
		// save the instance
		m_hInstance = hInstance;

		// tell it this is next created dialog
		g_DialogMap.AddNext(this);

		// create the dialog
		int nResult = DialogBox(hInstance, m_lpTemplate, hWndParent, DialogMap::DialogProc);

		// remove it from the list
		g_DialogMap.Remove(this);

		return nResult;
	}
};
