/****************************************************************************
** $Id: qt/src/tools/qqueue.h   2.3.10   edited 2005-01-24 $
**
** Definition of QQueue template/macro class
**
** Created : 920917
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

#ifndef QQUEUE_H
#define QQUEUE_H

#ifndef QT_H
#include "qglist.h"
#endif // QT_H


template<class type> class QQueue : private QGList
{
public:
    QQueue()				{ }
    QQueue( const QQueue<type> &q ) : QGList(q) { }
   ~QQueue()				{ }
    QQueue<type>& operator=(const QQueue<type> &q)
			{ }
    bool  autoDelete() const		{ }
    void  setAutoDelete( bool del )	{ }
    uint  count()   const		{ }
    bool  isEmpty() const		{ }
    void  enqueue( const type *d )	{ }
    type *dequeue()			{ }
    bool  remove()			{ }
    void  clear()			{ }
    type *head()    const		{ }
	  operator type *() const	{ }
    type *current() const		{ }
private:
    void  deleteItem( Item d );
};

#if defined(Q_DELETING_VOID_UNDEFINED)
template<> inline void QQueue<void>::deleteItem( Item )
{ }
#endif

template<class type> inline void QQueue<type>::deleteItem( QCollection::Item d )
{ }


#endif // QQUEUE_H
