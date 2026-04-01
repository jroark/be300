/****************************************************************************
** $Id: qt/src/tools/qcache.h   2.3.10   edited 2005-01-24 $
**
** Definition of QCache template class
**
** Created : 950209
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

#ifndef QCACHE_H
#define QCACHE_H

#ifndef QT_H
#include "qgcache.h"
#endif // QT_H


template<class type> class Q_EXPORT QCache : public QGCache
{
public:
    QCache( const QCache<type> &c ) : QGCache(c) { }
    QCache( int maxCost=100, int size=17, bool caseSensitive=TRUE )
	: QGCache( maxCost, size, StringKey, caseSensitive, FALSE ) { }
   ~QCache()				{ }
    QCache<type> &operator=( const QCache<type> &c )
			{ }
    int	  maxCost()   const		{ }
    int	  totalCost() const		{ }
    void  setMaxCost( int m )		{ }
    uint  count()     const		{ }
    uint  size()      const		{ }
    bool  isEmpty()   const		{ }
    void  clear()			{ }
    bool  insert( const QString &k, const type *d, int c=1, int p=0 )
			{ }
    bool  remove( const QString &k )
			{ }
    type *take( const QString &k )
			{ }
    type *find( const QString &k, bool ref=TRUE ) const
			{ }
    type *operator[]( const QString &k ) const
			{ }
    void  statistics() const	      { }
private:
    void  deleteItem( Item d );
};

#if defined(Q_DELETING_VOID_UNDEFINED)
template<> inline void QCache<void>::deleteItem( Item )
{ }
#endif

template<class type> inline void QCache<type>::deleteItem( QCollection::Item d )
{ }



template<class type> class Q_EXPORT QCacheIterator : public QGCacheIterator
{
public:
    QCacheIterator( const QCache<type> &c ):QGCacheIterator((QGCache &)c) { }
    QCacheIterator( const QCacheIterator<type> &ci)
				: QGCacheIterator( (QGCacheIterator &)ci ) { }
    QCacheIterator<type> &operator=(const QCacheIterator<type>&ci)
	{ }
    uint  count()   const     { }
    bool  isEmpty() const     { }
    bool  atFirst() const     { }
    bool  atLast()  const     { }
    type *toFirst()	      { }
    type *toLast()	      { }
    operator type *() const   { }
    type *current()   const   { }
    QString currentKey() const{ }
    type *operator()()	      { }
    type *operator++()	      { }
    type *operator+=(uint j)  { }
    type *operator--()	      { }
    type *operator-=(uint j)  { }
};


#endif // QCACHE_H
