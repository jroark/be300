/****************************************************************************
** $Id: qt/src/tools/qtl.h   2.3.10   edited 2005-01-24 $
**
** Definition of Qt template library classes
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
#ifndef QTL_H
#define QTL_H

#ifndef QT_H
#include "qtextstream.h"
#include "qstring.h"
#endif // QT_H

#ifndef QT_NO_TEXTSTREAM
template <class T>
class QTextOStreamIterator
{
protected:
    QTextOStream& stream;
    QString separator;

public:
    QTextOStreamIterator( QTextOStream& s) : stream( s ) { }
    QTextOStreamIterator( QTextOStream& s, const QString& sep )
	: stream( s ), separator( sep )  { }
    QTextOStreamIterator<T>& operator= ( const T& x ) { }
    QTextOStreamIterator<T>& operator*() { }
    QTextOStreamIterator<T>& operator++() { }
    QTextOStreamIterator<T>& operator++(int) { }
};
#endif //QT_NO_TEXTSTREAM

template <class InputIterator, class OutputIterator>
inline OutputIterator qCopy( InputIterator _begin, InputIterator _end,
			     OutputIterator _dest )
{ }


template <class T>
inline void qSwap( T& _value1, T& _value2 )
{ }


template <class InputIterator>
inline void qBubbleSort( InputIterator b, InputIterator e )
{ }


template <class Container>
inline void qBubbleSort( Container &c )
{ }


template <class Value>
inline void qHeapSortPushDown( Value* heap, int first, int last )
{ }


template <class InputIterator, class Value>
inline void qHeapSortHelper( InputIterator b, InputIterator e, Value, uint n )
{ }


template <class InputIterator>
inline void qHeapSort( InputIterator b, InputIterator e )
{ }


template <class Container>
inline void qHeapSort( Container &c )
{ }

#endif
