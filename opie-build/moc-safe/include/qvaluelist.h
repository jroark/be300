/****************************************************************************
** $Id: qt/src/tools/qvaluelist.h   2.3.10   edited 2005-01-24 $
**
** Definition of QValueList class
**
** Created : 990406
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

#ifndef QVALUELIST_H
#define QVALUELIST_H

#ifndef QT_H
#include "qshared.h"
#include "qdatastream.h"
#endif // QT_H

#if defined(_CC_MSVC_)
#pragma warning(disable:4284) // "return type for operator -> is not a UDT"
#endif

template <class T>
class Q_EXPORT QValueListNode
{
public:
    QValueListNode( const T& t ) : data( t ) { }
    QValueListNode() { }
#if defined(Q_TEMPLATEDLL)
    // Workaround MS bug in memory de/allocation in DLL vs. EXE 
    virtual ~QValueListNode() { }
#endif

    QValueListNode<T>* next;
    QValueListNode<T>* prev;
    T data;
};

template<class T>
class Q_EXPORT QValueListIterator
{
 public:
    /**
     * Typedefs
     */
    typedef QValueListNode<T>* NodePtr;

    /**
     * Variables
     */
    NodePtr node;

    /**
     * Functions
     */
    QValueListIterator() : node( 0 ) { }
    QValueListIterator( NodePtr p ) : node( p ) { }
    QValueListIterator( const QValueListIterator<T>& it ) : node( it.node ) { }

    bool operator==( const QValueListIterator<T>& it ) const { }
    bool operator!=( const QValueListIterator<T>& it ) const { }
    const T& operator*() const { }
    T& operator*() { }

    // Compilers are too dumb to understand this for QValueList<int>
    //T* operator->() const { }

    QValueListIterator<T>& operator++() { }

    QValueListIterator<T> operator++(int) { }

    QValueListIterator<T>& operator--() { }

    QValueListIterator<T> operator--(int) { }
};

template<class T>
class Q_EXPORT QValueListConstIterator
{
 public:
    /**
     * Typedefs
     */
    typedef QValueListNode<T>* NodePtr;

    /**
     * Variables
     */
    NodePtr node;

    /**
     * Functions
     */
    QValueListConstIterator() : node( 0 ) { }
    QValueListConstIterator( NodePtr p ) : node( p ) { }
    QValueListConstIterator( const QValueListConstIterator<T>& it ) : node( it.node ) { }
    QValueListConstIterator( const QValueListIterator<T>& it ) : node( it.node ) { }

    bool operator==( const QValueListConstIterator<T>& it ) const { }
    bool operator!=( const QValueListConstIterator<T>& it ) const { }
    const T& operator*() const { }

    // Compilers are too dumb to understand this for QValueList<int>
    //const T* operator->() const { }

    QValueListConstIterator<T>& operator++() { }

    QValueListConstIterator<T> operator++(int) { }

    QValueListConstIterator<T>& operator--() { }

    QValueListConstIterator<T> operator--(int) { }
};

template <class T>
class Q_EXPORT QValueListPrivate : public QShared
{
public:
    /**
     * Typedefs
     */
    typedef QValueListIterator<T> Iterator;
    typedef QValueListConstIterator<T> ConstIterator;
    typedef QValueListNode<T> Node;
    typedef QValueListNode<T>* NodePtr;

    /**
     * Functions
     */
    QValueListPrivate() { }
    QValueListPrivate( const QValueListPrivate<T>& _p ) : QShared() { }

    void derefAndDelete() // ### hack to get around hp-cc brain damage
    { }

#if defined(Q_TEMPLATEDLL)
    // Workaround MS bug in memory de/allocation in DLL vs. EXE 
    virtual
#endif
    ~QValueListPrivate() { }

    Iterator insert( Iterator it, const T& x ) { }

    Iterator remove( Iterator it ) { }

    NodePtr find( NodePtr start, const T& x ) const { }

    int findIndex( NodePtr start, const T& x ) const { }

    uint contains( const T& x ) const { }

    void remove( const T& x ) { }

    NodePtr at( uint i ) const { }

    void clear() { }

    NodePtr node;
    uint nodes;
};

template <class T>
class Q_EXPORT QValueList
{
public:
    /**
     * Typedefs
     */
    typedef QValueListIterator<T> Iterator;
    typedef QValueListConstIterator<T> ConstIterator;
    typedef T ValueType;

    /**
     * API
     */
    QValueList() { }
    QValueList( const QValueList<T>& l ) { }
    ~QValueList() { }

    QValueList<T>& operator= ( const QValueList<T>& l )
    { }

    QValueList<T> operator+ ( const QValueList<T>& l ) const
    { }

    QValueList<T>& operator+= ( const QValueList<T>& l )
    { }

    bool operator== ( const QValueList<T>& l ) const
    { }

    bool operator!= ( const QValueList<T>& l ) const { }

    Iterator begin() { }
    ConstIterator begin() const { }
    Iterator end() { }
    ConstIterator end() const { }
    Iterator fromLast() { }
    ConstIterator fromLast() const { }

    bool isEmpty() const { }

    Iterator insert( Iterator it, const T& x ) { }

    Iterator append( const T& x ) { }
    Iterator prepend( const T& x ) { }

    Iterator remove( Iterator it ) { }
    void remove( const T& x ) { }

    T& first() { }
    const T& first() const { }
    T& last() { }
    const T& last() const { }

    T& operator[] ( uint i ) { }
    const T& operator[] ( uint i ) const { }
    Iterator at( uint i ) { }
    ConstIterator at( uint i ) const { }
    Iterator find ( const T& x ) { }
    ConstIterator find ( const T& x ) const { }
    Iterator find ( Iterator it, const T& x ) { }
    ConstIterator find ( ConstIterator it, const T& x ) const { }
    int findIndex( const T& x ) const { }
    uint contains( const T& x ) const { }

    uint count() const { }

    void clear() { }


    QValueList<T>& operator+= ( const T& x )
    { }
    QValueList<T>& operator<< ( const T& x )
    { }


protected:
    /**
     * Helpers
     */
    void detach() { }

    /**
     * Variables
     */
    QValueListPrivate<T>* sh;
};

#if defined(Q_TEMPLATEDLL)
template class Q_EXPORT QValueList<int>;
#endif

#ifndef QT_NO_DATASTREAM
template<class T>
inline QDataStream& operator>>( QDataStream& s, QValueList<T>& l )
{ }

template<class T>
inline QDataStream& operator<<( QDataStream& s, const QValueList<T>& l )
{ }
#endif // QT_NO_DATASTREAM
#endif // QVALUELIST_H
