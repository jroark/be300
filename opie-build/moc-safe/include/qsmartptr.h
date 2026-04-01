/****************************************************************************
** $Id: qt/src/tools/qsmartptr.h   2.3.10   edited 2005-01-24 $
**
** Definition of QSmartPtr class
**
** Created : 990128
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
#ifndef QSMARTPTR_H
#define QSMARTPTR_H

#ifndef QT_H
#include "qshared.h"
#endif // QT_H


#if defined(QT_CLEAN_NAMESPACE) && (QT_CLEAN_NAMESPACE >= 220)
#error "QSmartPtr will be removed in Qt 3.0"
#endif
#if defined(_CC_GNU_)
#warning "QSmartPtr will be removed in Qt 3.0"
#endif


//
//  W A R N I N G
//  -------------
//
// This template was added to the public releases by mistake. It has
// never been mentioned in any Qt documentation, has never been
// announced, and will go away again in Qt 3.0.
//
// We mean it.
//
// If you need QSmartPtr, write to qt-bugs@troll.no and we'll give you
// a BSD-licensed copy of it.
//
//

template< class T >
class QSmartPtrPrivate : public QShared
{
public:
    QSmartPtrPrivate( T* t ) : QShared() { }
    ~QSmartPtrPrivate() { }

    T* addr;
};

template< class T >
struct QSmartPtr
{
public:
    QSmartPtr() { }
    QSmartPtr( T* t ) { }
    QSmartPtr( const QSmartPtr& p ) { }
    ~QSmartPtr() { }

    QSmartPtr<T>& operator= ( const QSmartPtr<T>& p ) { }
    QSmartPtr<T>& operator= ( T* p ) { }
    bool operator== ( const QSmartPtr<T>& p ) const
	{ }
    bool operator!= ( const QSmartPtr<T>& p ) const
	{ }
    bool operator== ( const T* p ) const { }
    bool operator!= ( const T* p ) const { }
    bool operator!() const { }
    operator bool() const { }
    operator T*() { }
    operator const T*() const { }

    const T& operator*() const { }
    T& operator*() { }
    const T* operator->() const { }
    T* operator->() { }

private:
    QSmartPtrPrivate<T>* ptr;
};

#endif
