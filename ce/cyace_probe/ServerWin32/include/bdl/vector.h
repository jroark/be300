// vector.h

// Written by Bradley D. LaRonde, brad@ltc.com.
// Copyright (C) 1999 Bradley D. LaRonde.

// This is a very very lame vector template for Windows CE
// (which doesn't have exception handling nor the STL)  :-(

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

template<class T> class Vector
{
private:
	T* m_Elements[100];
	int m_nSize;

public:
	Vector() : m_nSize(0)
	{
	}

	void Add(T* pt)
	{
		m_Elements[m_nSize++] = pt;
	}

	T* At(int n)
	{
		return m_Elements[n];
	}

	int Size()
	{
		return m_nSize;
	}
};

