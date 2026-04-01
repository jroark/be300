/****************************************************************************
** $Id: qt/src/kernel/qsize.h   2.3.10   edited 2005-01-24 $
**
** Definition of QSize class
**
** Created : 931028
**
** Copyright (C) 1992-2000 Trolltech AS.  All rights reserved.
**
** This file is part of the kernel module of the Qt GUI Toolkit.
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

#ifndef QSIZE_H
#define QSIZE_H

#ifndef QT_H
#include "qpoint.h"
#endif // QT_H


class Q_EXPORT QSize
{
public:
    QSize();
    QSize( int w, int h );

    bool   isNull()	const;
    bool   isEmpty()	const;
    bool   isValid()	const;

    int	   width()	const;
    int	   height()	const;
    void   setWidth( int w );
    void   setHeight( int h );
    void   transpose();

    QSize expandedTo( const QSize & ) const;
    QSize boundedTo( const QSize & ) const;

    QCOORD &rwidth();
    QCOORD &rheight();

    QSize &operator+=( const QSize & );
    QSize &operator-=( const QSize & );
    QSize &operator*=( int c );
    QSize &operator*=( double c );
    QSize &operator/=( int c );
    QSize &operator/=( double c );

    friend inline bool	operator==( const QSize &, const QSize & );
    friend inline bool	operator!=( const QSize &, const QSize & );
    friend inline QSize operator+( const QSize &, const QSize & );
    friend inline QSize operator-( const QSize &, const QSize & );
    friend inline QSize operator*( const QSize &, int );
    friend inline QSize operator*( int, const QSize & );
    friend inline QSize operator*( const QSize &, double );
    friend inline QSize operator*( double, const QSize & );
    friend inline QSize operator/( const QSize &, int );
    friend inline QSize operator/( const QSize &, double );

private:
    static void warningDivByZero();

    QCOORD wd;
    QCOORD ht;
};


/*****************************************************************************
  QSize stream functions
 *****************************************************************************/

Q_EXPORT QDataStream &operator<<( QDataStream &, const QSize & );
Q_EXPORT QDataStream &operator>>( QDataStream &, QSize & );


/*****************************************************************************
  QSize inline functions
 *****************************************************************************/

inline QSize::QSize()
{ }

inline QSize::QSize( int w, int h )
{ }

inline bool QSize::isNull() const
{ }

inline bool QSize::isEmpty() const
{ }

inline bool QSize::isValid() const
{ }

inline int QSize::width() const
{ }

inline int QSize::height() const
{ }

inline void QSize::setWidth( int w )
{ }

inline void QSize::setHeight( int h )
{ }

inline QCOORD &QSize::rwidth()
{ }

inline QCOORD &QSize::rheight()
{ }

inline QSize &QSize::operator+=( const QSize &s )
{ }

inline QSize &QSize::operator-=( const QSize &s )
{ }

inline QSize &QSize::operator*=( int c )
{ }

inline QSize &QSize::operator*=( double c )
{ }

inline bool operator==( const QSize &s1, const QSize &s2 )
{ }

inline bool operator!=( const QSize &s1, const QSize &s2 )
{ }

inline QSize operator+( const QSize & s1, const QSize & s2 )
{ }

inline QSize operator-( const QSize &s1, const QSize &s2 )
{ }

inline QSize operator*( const QSize &s, int c )
{ }

inline QSize operator*( int c, const QSize &s )
{ }

inline QSize operator*( const QSize &s, double c )
{ }

inline QSize operator*( double c, const QSize &s )
{ }

inline QSize &QSize::operator/=( int c )
{ }

inline QSize &QSize::operator/=( double c )
{ }

inline QSize operator/( const QSize &s, int c )
{ }

inline QSize operator/( const QSize &s, double c )
{ }

inline QSize QSize::expandedTo( const QSize & otherSize ) const
{ }

inline QSize QSize::boundedTo( const QSize & otherSize ) const
{ }


#endif // QSIZE_H
