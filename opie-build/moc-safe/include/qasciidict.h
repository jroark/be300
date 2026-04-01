/****************************************************************************
** $Id: qt/src/tools/qasciidict.h   2.3.10   edited 2005-01-24 $
**
** Definition of QAsciiDict template class
**
** Created : 920821
**
** Copyright (C) 1992-2000 Trolltech AS.  All rights reserved.
**
** This file is part of the tools module of the Qt GUI Toolkit.
**
** This file may be distributed under the terms of the Q Public License
** as defined by Trolltech AS of Norway and appearing in the file
** LICENSE.QPL included in the packaging of this file.
**
** This file may be distributed and/or modified under the terms of the
** GNU General Public License version 2 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.
**
** Licensees holding valid Qt Enterprise Edition or Qt Professional Edition
** licenses may use this file in accordance with the Qt Commercial License
** Agreement provided with the Software.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
** See http://www.trolltech.com/pricing.html or email sales@trolltech.com for
**   information about Qt Commercial License Agreements.
** See http://www.trolltech.com/qpl/ for QPL licensing information.
** See http://www.trolltech.com/gpl/ for GPL licensing information.
**
** Contact info@trolltech.com if any conditions of this licensing are
** not clear to you.
**
**********************************************************************/

#ifndef QASCIIDICT_H
#define QASCIIDICT_H

#ifndef QT_H
#include "qgdict.h"
#endif // QT_H


template<class type> class Q_EXPORT QAsciiDict : public QGDict
{
public:
    QAsciiDict(int size=17, bool caseSensitive=TRUE, bool copyKeys=TRUE )
	: QGDict(size,AsciiKey,caseSensitive,copyKeys) { }
    QAsciiDict( const QAsciiDict<type> &d ) : QGDict(d) { }
   ~QAsciiDict()			{ }
    QAsciiDict<type> &operator=(const QAsciiDict<type> &d)
			{ }
    uint  count()   const		{ }
    uint  size()    const		{ }
    bool  isEmpty() const		{ }

    void  insert( const char *k, const type *d )
					{ }
    void  replace( const char *k, const type *d )
					{ }
    bool  remove( const char *k )	{ }
    type *take( const char *k )		{ }
    type *find( const char *k ) const
		{ }
    type *operator[]( const char *k ) const
		{ }

    void  clear()			{ }
    void  resize( uint n )		{ }
    void  statistics() const		{ }
private:
    void  deleteItem( Item d );
};

#if defined(Q_DELETING_VOID_UNDEFINED)
template<> inline void QAsciiDict<void>::deleteItem( Item )
{ }
#endif

template<class type> inline void QAsciiDict<type>::deleteItem( QCollection::Item d )
{ }


template<class type> class Q_EXPORT QAsciiDictIterator : public QGDictIterator
{
public:
    QAsciiDictIterator(const QAsciiDict<type> &d)
	: QGDictIterator((QGDict &)d) { }
   ~QAsciiDictIterator()      { }
    uint  count()   const     { }
    bool  isEmpty() const     { }
    type *toFirst()	      { }
    operator type *() const   { }
    type   *current() const   { }
    const char *currentKey() const { }
    type *operator()()	      { }
    type *operator++()	      { }
    type *operator+=(uint j)  { }
};


#endif // QASCIIDICT_H
