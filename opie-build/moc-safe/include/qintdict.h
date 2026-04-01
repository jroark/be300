/****************************************************************************
** $Id: qt/src/tools/qintdict.h   2.3.10   edited 2005-01-24 $
**
** Definition of QIntDict template class
**
** Created : 940624
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

#ifndef QINTDICT_H
#define QINTDICT_H

#ifndef QT_H
#include "qgdict.h"
#endif // QT_H


template<class type> class Q_EXPORT QIntDict : public QGDict
{
public:
    QIntDict(int size=17) : QGDict(size,IntKey,0,0) { }
    QIntDict( const QIntDict<type> &d ) : QGDict(d) { }
   ~QIntDict()				{ }
    QIntDict<type> &operator=(const QIntDict<type> &d)
			{ }
    uint  count()   const		{ }
    uint  size()    const		{ }
    bool  isEmpty() const		{ }
    void  insert( long k, const type *d )
					{ }
    void  replace( long k, const type *d )
					{ }
    bool  remove( long k )		{ }
    type *take( long k )		{ }
    type *find( long k ) const
		{ }
    type *operator[]( long k ) const
		{ }
    void  clear()			{ }
    void  resize( uint n )		{ }
    void  statistics() const		{ }
private:
    void  deleteItem( Item d );
};

#if defined(Q_DELETING_VOID_UNDEFINED)
template<> inline void QIntDict<void>::deleteItem( QCollection::Item )
{ }
#endif

template<class type> inline void QIntDict<type>::deleteItem( QCollection::Item d )
{ }

template<class type> class Q_EXPORT QIntDictIterator : public QGDictIterator
{
public:
    QIntDictIterator(const QIntDict<type> &d) :QGDictIterator((QGDict &)d) { }
   ~QIntDictIterator()	      { }
    uint  count()   const     { }
    bool  isEmpty() const     { }
    type *toFirst()	      { }
    operator type *()  const  { }
    type *current()    const  { }
    long  currentKey() const  { }
    type *operator()()	      { }
    type *operator++()	      { }
    type *operator+=(uint j)  { }
};


#endif // QINTDICT_H
