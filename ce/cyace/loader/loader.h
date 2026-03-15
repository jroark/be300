/*
 * $Id: loader.h,v 1.3 2004/08/28 21:29:33 be300 Exp $
 * 
 * loader.h
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

class BootConfig;
class ImageSection;


class Loader
{
	private:
		int m_nVerboseLevel;

	enum Processor
	{
		PROCESSOR_UNKNOWN, NEC_VR41XX, PHILLIPS_PR31700
	};

	enum Platform
	{
		PLATFORM_UNKNOWN,
		CASIO_E10, CASIO_E15, CASIO_E105,
		EVEREX_FREESTYLE,
		NEC_MOBILPRO,
		PHILIPS_NINO,
		VADEM_CLIO
	};

	Processor GetProcessor();
	Platform GetPlatform();
	BOOL GetProcessorSpecificSettings(unsigned int* pps, startprog_t* psp);
	void Load(ImageSection* pSection, LPCTSTR pszParameters, LPCTSTR pszBootURL);
	void LoadURL(ImageSection* pSection, LPCTSTR pszParameters, LPCTSTR pszBootURL);
	void GetSystemProcessorInfo(LPTSTR psz);
	BOOL VerifyProcessorSupport();

public:
	int Main();
};
