/****************************************************************************
** $Id: qt/src/tools/qptrdict.h   2.3.10   edited 2005-01-24 $
**
** Definition of QPtrDict template class
**
** Created : 970415
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

#ifndef QPTRDICT_H
#define QPTRDICT_H

#ifndef QT_H
#include "qgdict.h"
#endif // QT_H


template<class type> class Q_EXPORT QPtrDict : public QGDict
{
public:
    QPtrDict(int size=17) : QGDict(size,PtrKey,0,0) { }
    QPtrDict( const QPtrDict<type> &d ) : QGDict(d) { }
   ~QPtrDict()				{ }
    QPtrDict<type> &operator=(const QPtrDict<type> &d)
			{ }
    uint  count()   const		{ }
    uint  size()    const		{ }
    bool  isEmpty() const		{ }
    void  insert( void *k, const type *d )
					{ }
    void  replace( void *k, const type *d )
					{ }
    bool  remove( void *k )		{ }
    type *take( void *k )		{ }
    type *find( void *k ) const
		{ }
    type *operator[]( void *k ) const
		{ }
    void  clear()			{ }
    void  resize( uint n )		{ }
    void  statistics() const		{ }
private:
    void  deleteItem( Item d );
};

#if defined(Q_DELETING_VOID_UNDEFINED)
template<> inline void QPtrDict<void>::deleteItem( QCollection::Item )
{ }
#endif

template<class type> inline void QPtrDict<type>::deleteItem( QCollection::Item d )
{ }


template<class type> class Q_EXPORT QPtrDictIterator : public QGDictIterator
{
public:
    QPtrDictIterator(const QPtrDict<type> &d) :QGDictIterator((QGDict &)d) { }
   ~QPtrDictIterator()	      { }
    uint  count()   const     { }
    bool  isEmpty() const     { }
    type *toFirst()	      { }
    operator type *()  const  { }
    type *current()    const  { }
    void *currentKey() const  { }
    type *operator()()	      { }
    type *operator++()	      { }
    type *operator+=(uint j)  { }
};


#endif // QPTRDICT_H
