/****************************************************************************
** $Id: qt/src/tools/qlist.h   2.3.10   edited 2005-01-24 $
**
** Definition of QList template/macro class
**
** Created : 920701
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

#ifndef QLIST_H
#define QLIST_H

#ifndef QT_H
#include "qglist.h"
#endif // QT_H


template<class type> class Q_EXPORT QList : public QGList
{
public:
    QList()				{ }
    QList( const QList<type> &l ) : QGList(l) { }
   ~QList()				{ }
    QList<type> &operator=(const QList<type> &l)
			{ }
    bool operator==( const QList<type> &list ) const
    { }
    uint  count()   const		{ }
    bool  isEmpty() const		{ }
    bool  insert( uint i, const type *d){ }
    void  inSort( const type *d )	{ }
    void  prepend( const type *d )	{ }
    void  append( const type *d )	{ }
    bool  remove( uint i )		{ }
    bool  remove()			{ }
    bool  remove( const type *d )	{ }
    bool  removeRef( const type *d )	{ }
    void  removeNode( QLNode *n )	{ }
    bool  removeFirst()			{ }
    bool  removeLast()			{ }
    type *take( uint i )		{ }
    type *take()			{ }
    type *takeNode( QLNode *n )		{ }
    void  clear()			{ }
    void  sort()			{ }
    int	  find( const type *d )		{ }
    int	  findNext( const type *d )	{ }
    int	  findRef( const type *d )	{ }
    int	  findNextRef( const type *d ){ }
    uint  contains( const type *d ) const { }
    uint  containsRef( const type *d ) const
					{ }
    type *at( uint i )			{ }
    int	  at() const			{ }
    type *current()  const		{ }
    QLNode *currentNode()  const	{ }
    type *getFirst() const		{ }
    type *getLast()  const		{ }
    type *first()			{ }
    type *last()			{ }
    type *next()			{ }
    type *prev()			{ }
    void  toVector( QGVector *vec )const{ }
private:
    void  deleteItem( QCollection::Item d );
};

#if defined(Q_DELETING_VOID_UNDEFINED)
template<> inline void QList<void>::deleteItem( QCollection::Item )
{ }
#endif

template<class type> inline void QList<type>::deleteItem( QCollection::Item d )
{ }


template<class type> class Q_EXPORT QListIterator : public QGListIterator
{
public:
    QListIterator(const QList<type> &l) :QGListIterator((QGList &)l) { }
   ~QListIterator()	      { }
    uint  count()   const     { }
    bool  isEmpty() const     { }
    bool  atFirst() const     { }
    bool  atLast()  const     { }
    type *toFirst()	      { }
    type *toLast()	      { }
    operator type *() const   { }
    type *operator*()         { }

    // No good, since QList<char> (ie. QStrList fails...
    //
    // MSVC++ gives warning
    // Sunpro C++ 4.1 gives error
    //    type *operator->()        { }

    type *current()   const   { }
    type *operator()()	      { }
    type *operator++()	      { }
    type *operator+=(uint j)  { }
    type *operator--()	      { }
    type *operator-=(uint j)  { }
    QListIterator<type>& operator=(const QListIterator<type>&it)
			      { }
};


#endif // QLIST_H
