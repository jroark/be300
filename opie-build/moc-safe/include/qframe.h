/****************************************************************************
** $Id: qt/src/widgets/qframe.h   2.3.10   edited 2005-01-24 $
**
** Definition of QFrame widget class
**
** Created : 950201
**
** Copyright (C) 1992-2000 Trolltech AS.  All rights reserved.
**
** This file is part of the widgets module of the Qt GUI Toolkit.
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

#ifndef QFRAME_H
#define QFRAME_H

#ifndef QT_H
#include "qwidget.h"
#endif // QT_H

#ifndef QT_NO_FRAME

class Q_EXPORT QFrame : public QWidget			// frame class
{
    Q_OBJECT
    Q_ENUMS( Shape Shadow )
    Q_PROPERTY( int frameWidth READ frameWidth )
    Q_PROPERTY( QRect contentsRect READ contentsRect )
    Q_PROPERTY( Shape frameShape READ frameShape WRITE setFrameShape )
    Q_PROPERTY( Shadow frameShadow READ frameShadow WRITE setFrameShadow )
    Q_PROPERTY( int lineWidth READ lineWidth WRITE setLineWidth )
    Q_PROPERTY( int margin READ margin WRITE setMargin )
    Q_PROPERTY( int midLineWidth READ midLineWidth WRITE setMidLineWidth )
    Q_PROPERTY( QRect frameRect READ frameRect WRITE setFrameRect DESIGNABLE false )

public:
    QFrame( QWidget *parent=0, const char *name=0, WFlags f=0,
	    bool = TRUE );

    int		frameStyle()	const;
    virtual void setFrameStyle( int );

    int		frameWidth()	const;
    QRect	contentsRect()	const;

#if 1 // OBSOLETE, provided for compatibility
    bool	lineShapesOk()	const { }
#endif

    QSize	sizeHint() const;
    QSizePolicy sizePolicy() const;

    enum Shape { };
    enum Shadow { };			// mask for the shadow

    Shape	frameShape()	const;
    void	setFrameShape( Shape );
    Shadow	frameShadow()	const;
    void	setFrameShadow( Shadow );

    int		lineWidth()	const;
    virtual void setLineWidth( int );

    int		margin()	const;
    virtual void setMargin( int );

    int		midLineWidth()	const;
    virtual void setMidLineWidth( int );

    QRect	frameRect()	const;
    virtual void setFrameRect( const QRect & );

protected:
    void	paintEvent( QPaintEvent * );
    void	resizeEvent( QResizeEvent * );
    virtual void drawFrame( QPainter * );
    virtual void drawContents( QPainter * );
    virtual void frameChanged();
    void	updateMask();
    virtual void drawFrameMask( QPainter * );
    virtual void drawContentsMask( QPainter * );

private:
    void	updateFrameWidth();
#ifdef QT_KEYPAD_MODE
    friend class QFrameEventHandler;
    bool eventPrivate(QEvent *);
#endif
    QRect	frect;
    int		fstyle;
    short	lwidth;
    short	mwidth;
    short	mlwidth;
    short	fwidth;

    void * d;
private:	// Disabled copy constructor and operator=
#if defined(Q_DISABLE_COPY)
    QFrame( const QFrame & );
    QFrame &operator=( const QFrame & );
#endif
};


inline int QFrame::frameStyle() const
{ }

inline QFrame::Shape QFrame::frameShape() const
{ }

inline QFrame::Shadow QFrame::frameShadow() const
{ }

inline void QFrame::setFrameShape( QFrame::Shape s )
{ }

inline void QFrame::setFrameShadow( QFrame::Shadow s )
{ }

inline int QFrame::lineWidth() const
{ }

inline int QFrame::midLineWidth() const
{ }

inline int QFrame::margin() const
{ }

inline int QFrame::frameWidth() const
{ }


#endif // QT_NO_FRAME

#endif // QFRAME_H
