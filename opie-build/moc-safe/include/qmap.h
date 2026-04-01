/****************************************************************************
** $Id: qt/src/tools/qmap.h   2.3.10   edited 2005-01-24 $
**
** Definition of QMap class
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

#ifndef QMAP_H
#define QMAP_H

#ifndef QT_H
#include "qshared.h"
#include "qdatastream.h"
#endif // QT_H


struct QMapNodeBase
{ };


template <class K, class T>
struct QMapNode : public QMapNodeBase
{ };


template<class K, class T>
class Q_EXPORT QMapIterator
{
 public:
    /**
     * Typedefs
     */
    typedef QMapNode< K, T >* NodePtr;

    /**
     * Variables
     */
    QMapNode<K,T>* node;

    /**
     * Functions
     */
    QMapIterator() : node( 0 ) { }
    QMapIterator( QMapNode<K,T>* p ) : node( p ) { }
    QMapIterator( const QMapIterator<K,T>& it ) : node( it.node ) { }

    bool operator==( const QMapIterator<K,T>& it ) const { }
    bool operator!=( const QMapIterator<K,T>& it ) const { }
    T& operator*() { }
    const T& operator*() const { }

    // Cannot have this - some compilers are too stupid
    //T* operator->() const { }

    const K& key() const { }
    T& data() { }
    const T& data() const { }

private:
    int inc() { }

    int dec() { }

public:
    QMapIterator<K,T>& operator++() { }

    QMapIterator<K,T> operator++(int) { }

    QMapIterator<K,T>& operator--() { }

    QMapIterator<K,T> operator--(int) { }
};

template<class K, class T>
class Q_EXPORT QMapConstIterator
{
 public:
    /**
     * Typedefs
     */
    typedef QMapNode< K, T >* NodePtr;

    /**
     * Variables
     */
    QMapNode<K,T>* node;

    /**
     * Functions
     */
    QMapConstIterator() : node( 0 ) { }
    QMapConstIterator( QMapNode<K,T>* p ) : node( p ) { }
    QMapConstIterator( const QMapConstIterator<K,T>& it ) : node( it.node ) { }
    QMapConstIterator( const QMapIterator<K,T>& it ) : node( it.node ) { }

    bool operator==( const QMapConstIterator<K,T>& it ) const { }
    bool operator!=( const QMapConstIterator<K,T>& it ) const { }
    const T& operator*()  const { }

    // Cannot have this - some compilers are too stupid
    //const T* operator->() const { }

    const K& key() const { }
    const T& data() const { }

private:
    int inc() { }

    int dec() { }

public:
    QMapConstIterator<K,T>& operator++() { }

    QMapConstIterator<K,T> operator++(int) { }

    QMapConstIterator<K,T>& operator--() { }

    QMapConstIterator<K,T> operator--(int) { }
};


class Q_EXPORT QMapPrivateBase : public QShared
{
public:
    QMapPrivateBase() { }
    QMapPrivateBase( const QMapPrivateBase* _map) { }

    /**
     * Implementations of basic tree algorithms
     */
    void rotateLeft( QMapNodeBase* x, QMapNodeBase*& root);
    void rotateRight( QMapNodeBase* x, QMapNodeBase*& root );
    void rebalance( QMapNodeBase* x, QMapNodeBase*& root );
    QMapNodeBase* removeAndRebalance( QMapNodeBase* z, QMapNodeBase*& root,
				      QMapNodeBase*& leftmost,
				      QMapNodeBase*& rightmost );

    /**
     * Variables
     */
    int node_count;
};


template <class Key, class T>
class QMapPrivate : public QMapPrivateBase
{
public:
    /**
     * Typedefs
     */
    typedef QMapIterator< Key, T > Iterator;
    typedef QMapConstIterator< Key, T > ConstIterator;
    typedef QMapNode< Key, T > Node;
    typedef QMapNode< Key, T >* NodePtr;

    /**
     * Functions
     */
    QMapPrivate() { }
    QMapPrivate( const QMapPrivate< Key, T >* _map ) : QMapPrivateBase( _map ) { }
    ~QMapPrivate() { }

    NodePtr copy( NodePtr p ) { }

    void clear() { }

    void clear( NodePtr p ) { }

    Iterator begin()	{ }
    Iterator end()	{ }
    ConstIterator begin() const { }
    ConstIterator end() const { }

    ConstIterator find(const Key& k) const { }

    void remove( Iterator it ) { }

#ifdef QT_QMAP_DEBUG
    void inorder( QMapNodeBase* x = 0, int level = 0 ){ }
#endif

    Iterator insertMulti(const Key& v){ }

    Iterator insertSingle( const Key& k ) { }

    Iterator insert( QMapNodeBase* x, QMapNodeBase* y, const Key& k ) { }

protected:
    /**
     * Helpers
     */
    const Key& key( QMapNodeBase* b ) const { }

    /**
     * Variables
     */
    NodePtr header;
};


template<class Key, class T>
class Q_EXPORT QMap
{
public:
    /**
     * Typedefs
     */
    typedef QMapIterator< Key, T > Iterator;
    typedef QMapConstIterator< Key, T > ConstIterator;
    typedef T ValueType;
    typedef QMapPrivate< Key, T > Priv;

    /**
     * API
     */
    QMap() { }
    QMap( const QMap<Key,T>& m ) { }
    ~QMap() { }

    QMap<Key,T>& operator= ( const QMap<Key,T>& m )
      { }

    Iterator begin() { }
    Iterator end() { }
    ConstIterator begin() const { }
    ConstIterator end() const { }

    Iterator find ( const Key& k )
	{ }
    ConstIterator find ( const Key& k ) const
	{ }
    T& operator[] ( const Key& k ) { }
    const T& operator[] ( const Key& k ) const
	{ }
    bool contains ( const Key& k ) const
	{ }
	//{ }

    uint count() const { }

    bool isEmpty() const { }

    Iterator insert( const Key& key, const T& value ) { }

    void remove( Iterator it ) { }
    void remove( const Key& k ) { }

    Iterator replace( const Key& k, const T& v ) { }

    void clear() { }

#if defined(Q_FULL_TEMPLATE_INSTANTIATION)
    bool operator==( const QMap<Key,T>& ) const { }
#endif

protected:
    /**
     * Helpers
     */
    void detach() { }

    Priv* sh;
};


#ifndef QT_NO_DATASTREAM
template<class Key, class T>
inline QDataStream& operator>>( QDataStream& s, QMap<Key,T>& m ) { }


template<class Key, class T>
inline QDataStream& operator<<( QDataStream& s, const QMap<Key,T>& m ) { }
#endif

#endif // QMAP_H
