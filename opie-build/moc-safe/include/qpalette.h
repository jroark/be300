/****************************************************************************
** $Id: qt/src/kernel/qpalette.h   2.3.10   edited 2005-01-24 $
**
** Definition of QColorGroup and QPalette classes
**
** Created : 950323
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

#ifndef QPALETTE_H
#define QPALETTE_H

#ifndef QT_H
#include "qwindowdefs.h"
#include "qcolor.h"
#include "qshared.h"
#include "qbrush.h"
#endif // QT_H

#ifndef QT_NO_PALETTE
class QColorGroupPrivate;


class Q_EXPORT QColorGroup
{
public:
    QColorGroup();
    QColorGroup( const QColor &foreground, const QColor &button,
		 const QColor &light, const QColor &dark, const QColor &mid,
		 const QColor &text, const QColor &base );
    QColorGroup( const QBrush &foreground, const QBrush &button,
		 const QBrush &light, const QBrush &dark, const QBrush &mid,
		 const QBrush &text, const QBrush &bright_text,
		 const QBrush &base, const QBrush &background);
    QColorGroup( const QColorGroup & );

   ~QColorGroup();

    QColorGroup& operator =(const QColorGroup&);

    // Do not change the order, the serialization format depends on it
    enum ColorRole { };

    const QColor &color( ColorRole ) const;
    const QBrush &brush( ColorRole ) const;
    void setColor( ColorRole, const QColor & );
    void setBrush( ColorRole, const QBrush & );

    const QColor &foreground()	const	{ }
    const QColor &button()	const	{ }
    const QColor &light()	const	{ }
    const QColor &dark()	const	{ }
    const QColor &mid()		const	{ }
    const QColor &text()	const	{ }
    const QColor &base()	const	{ }
    const QColor &background()	const	{ }

    const QColor &midlight()	const	{ }
    const QColor &brightText()	const	{ }
    const QColor &buttonText()	const	{ }
    const QColor &shadow()	const	{ }
    const QColor &highlight()	const	{ }
    const QColor &highlightedText() const{ }

    bool	operator==( const QColorGroup &g ) const;
    bool	operator!=( const QColorGroup &g ) const
	{ }

private:
    QBrush *br;
    QColorGroupPrivate * d;

    friend class QPalette;
};


class Q_EXPORT QPalette
{
public:
    QPalette();
    QPalette( const QColor &button );
    QPalette( const QColor &button, const QColor &background );
    QPalette( const QColorGroup &active, const QColorGroup &disabled,
	      const QColorGroup &inactive );
    QPalette( const QPalette & );
   ~QPalette();
    QPalette &operator=( const QPalette & );

    enum ColorGroup { };

    const QColor &color( ColorGroup, QColorGroup::ColorRole ) const;
    const QBrush &brush( ColorGroup, QColorGroup::ColorRole ) const;
    void setColor( ColorGroup, QColorGroup::ColorRole, const QColor & );
    void setBrush( ColorGroup, QColorGroup::ColorRole, const QBrush & );

    void setColor( QColorGroup::ColorRole, const QColor & );
    void setBrush( QColorGroup::ColorRole, const QBrush & );

    QPalette	copy() const;

    const QColorGroup &active() const { }
    const QColorGroup &disabled() const { }
    const QColorGroup &inactive() const { }
    const QColorGroup &normal() const { } // obsolete

    void	setActive( const QColorGroup & );
    void	setDisabled( const QColorGroup & );
    void	setInactive( const QColorGroup & );
    void	setNormal( const QColorGroup & ); // obsolete

    bool	operator==( const QPalette &p ) const;
    bool	operator!=( const QPalette &p ) const
					{ }
    bool	isCopyOf( const QPalette & );

    int		serialNumber() const	{ }

private:
    void	detach();
    QBrush 	&directBrush( ColorGroup, QColorGroup::ColorRole ) const;

    struct QPalData : public QShared { } *data;
    static QPalData *defPalData;
    static void cleanupDefPal();
};


/*****************************************************************************
  QColorGroup/QPalette stream functions
 *****************************************************************************/

#ifndef QT_NO_DATASTREAM
Q_EXPORT QDataStream &operator<<( QDataStream &, const QColorGroup & );
Q_EXPORT QDataStream &operator>>( QDataStream &, QColorGroup & );

Q_EXPORT QDataStream &operator<<( QDataStream &, const QPalette & );
Q_EXPORT QDataStream &operator>>( QDataStream &, QPalette & );
#endif // QT_NO_DATASTREAM

#endif // QT_NO_PALETTE
#endif // QPALETTE_H
