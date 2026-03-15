/*
 * $Id: config.h,v 1.3 2004/08/28 21:29:33 be300 Exp $
 *
 * config.h
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

class ImageSection
{
private:
	TCHAR m_szImage[256];
	TCHAR m_szLabel[256];
	TCHAR m_szAppend[256];
	TCHAR m_szURL[256];

public:
	ImageSection(LPCTSTR pszImage)
	{
		_tcscpy(m_szImage, pszImage);

		// default label to image name
		_tcscpy(m_szLabel, pszImage);

		_tcscpy(m_szAppend, _T(""));
		_tcscpy(m_szURL, _T(""));
	}

	LPCTSTR GetImage()
	{
		return m_szImage;
	}

	void SetAppend(LPCTSTR psz)
	{
		_tcscpy(m_szAppend, psz);
	}

	LPCTSTR GetAppend()
	{
		return m_szAppend;
	}

	void SetLabel(LPCTSTR psz)
	{
		_tcscpy(m_szLabel, psz);
	}

	LPCTSTR GetLabel()
	{
		return m_szLabel;
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

class BootConfig
{
private:
	Vector<ImageSection> m_ImageSections;
	int m_nTimeout;

	BOOL Interpret(LPCTSTR pszEntry, LPCTSTR pszValue);

public:
	BootConfig();

	int GetTimeout()
	{
		return m_nTimeout;
	}

	Vector<ImageSection>* GetImageSections()
	{
		return &m_ImageSections;
	}

	BOOL Load(LPCTSTR pszFile);
	BOOL Save(LPCTSTR pszFile);
};
