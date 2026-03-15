// progress.h

// Written by Bradley D. LaRonde, brad@ltc.com.
// Copyright (C) 1999 Bradley D. LaRonde.

// A simple modeless progress dialog.

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

class ProgressDialog : public ModelessDialog
{
private:
	BOOL m_bCancel;
	BOOL m_bShowPercent;

	BOOL OnInitDialog()
	{
		// clear all the fields
		SetItemText(IDC_OPERATION, _T(""));
		SetItemText(IDC_STATUS, _T(""));
		SetItemText(IDC_PERCENT, _T(""));

		// set the range
		// this is the same as the default range - just here for clarity
		SendItemMessage(IDC_PROGRESS, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
		return TRUE;
	}

	BOOL OnCancel()
	{
		m_bCancel = TRUE;
		return TRUE;
	}

public:
	ProgressDialog(LPCTSTR pszTemplate = MAKEINTRESOURCE(IDD_PROGRESS)) :
		ModelessDialog(pszTemplate),
		m_bCancel(FALSE), m_bShowPercent(TRUE)
	{
	}

	void SetShowPercent(BOOL bEnable)
	{
		m_bShowPercent = bEnable;
	}

	BOOL ShouldCancel()
	{
		Refresh();
		return m_bCancel;
	}

	BOOL Update(int nPercentComplete, LPCTSTR pszStatus = 0, LPCTSTR pszOperation = 0)
	{
		if ( pszOperation != 0 )
		{
			SetItemText(IDC_OPERATION, pszOperation);
			UpdateItem(IDC_OPERATION);
		}

		if ( pszStatus != 0 )
		{
			SetItemText(IDC_STATUS, pszStatus);
			UpdateItem(IDC_STATUS);
		}

		if ( m_bShowPercent )
		{
			TCHAR szMsg[512];
			_stprintf(szMsg, _T("%d%% complete"), nPercentComplete);
			SetItemText(IDC_PERCENT, szMsg);
			UpdateItem(IDC_PERCENT);
		}

		// set the position
		SendItemMessage(IDC_PROGRESS, PBM_SETPOS, (WPARAM)nPercentComplete, 0);

		return ShouldCancel();
	}
};

class ProgressCallbackAdapter
{
protected:
	static ProgressDialog* m_pProgressDialog;

public:
	ProgressCallbackAdapter(ProgressDialog* p)
	{
		m_pProgressDialog = p;
	}

	static Update(int nPercentComplete, LPCTSTR pszStatus = 0,
		LPCTSTR pszOperation = 0)
	{
		return ProgressCallbackAdapter::m_pProgressDialog->
			Update(nPercentComplete, pszStatus, pszOperation);
	}
};

ProgressDialog* ProgressCallbackAdapter::m_pProgressDialog;
