/****************************************************************************
** $Id: qt/src/kernel/qfont.h   2.3.10   edited 2005-01-24 $
**
** Definition of QFont class
**
** Created : 940514
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

#ifndef QFONT_H
#define QFONT_H

#ifndef QT_H
#include "qwindowdefs.h"
#include "qstring.h"
#endif // QT_H


class QStringList;
struct QFontDef;
struct QFontData;
class QFontInternal;
class QRenderedFont;

class Q_EXPORT QFont					// font class
{
public:
    enum CharSet   { };
    enum StyleHint { };
    enum StyleStrategy { };
    enum Weight	   { };
    QFont();					// default font
    QFont( const QString &family, int pointSize = 12,
	   int weight = Normal, bool italic = FALSE );
    QFont( const QString &family, int pointSize,
	   int weight, bool italic, CharSet charSet );
    QFont( const QFont & );
    ~QFont();
    QFont      &operator=( const QFont & );

    QString	family()	const;
    void	setFamily( const QString &);
    int		pointSize()	const;
    float	pointSizeFloat()	const;
    void	setPointSize( int );
    void	setPointSizeFloat( float );
    int		pixelSize() const;
    void	setPixelSize( int );
    void	setPixelSizeFloat( float );
    int		weight()	const;
    void	setWeight( int );
    bool	bold()		const;
    void	setBold( bool );
    bool	italic()	const;
    void	setItalic( bool );
    bool	underline()	const;
    void	setUnderline( bool );
    bool	strikeOut()	const;
    void	setStrikeOut( bool );
    bool	fixedPitch()	const;
    void	setFixedPitch( bool );
    StyleHint	styleHint()	const;
    void	setStyleHint( StyleHint );
    StyleStrategy styleStrategy() const;
    void	setStyleHint( StyleHint, StyleStrategy );
    CharSet	charSet()	const;
    void	setCharSet( CharSet );

    static CharSet charSetForLocale();

    bool	rawMode()      const;
    void	setRawMode( bool );

    bool	exactMatch()	const;

    bool	operator==( const QFont & ) const;
    bool	operator!=( const QFont & ) const;
    bool	isCopyOf( const QFont & ) const;

#if defined(_WS_WIN_)
    HFONT	handle() const;
#elif defined(_WS_MAC_)
    HANDLE      handle() const;
#elif defined(_WS_X11_)
    HANDLE	handle() const;
#elif defined(_WS_QWS_)
    HANDLE	handle() const;
#endif

    void	setRawName( const QString & );
    QString	rawName() const;

    QString	key() const;

    static QString encodingName( CharSet );

    static QFont defaultFont();
    static void setDefaultFont( const QFont & );

    static QString substitute( const QString &familyName );
    static void insertSubstitution( const QString&, const QString &);
    static void removeSubstitution( const QString &);
    static QStringList substitutions();

    static void initialize();
    static void locale_init();
    static void cleanup();
    static void cacheStatistics();

#if defined(_WS_QWS_)
    void qwsRenderToDisk(bool all=TRUE);
#endif

protected:
    bool	dirty()			const;

    QString	defaultFamily()		const;
    QString	lastResortFamily()	const;
    QString	lastResortFont()	const;
    int		deciPointSize()		const;

private:
    QFont( QFontData * );
    void	init();
    void	detach();
    void	initFontInfo() const;
    void	load() const;
#if defined(_WS_MAC_)
    void        macSetFont(void *);
#endif
#if defined(_WS_WIN_)
    HFONT	create( bool *, HDC=0, bool=FALSE ) const;
    void       *textMetric() const;
#endif

    friend class QFont_Private;
    friend class QFontInternal;
    friend class QFontMetrics;
    friend class QFontInfo;
    friend class QPainter;
#if defined(_WS_X11_) && defined(QT_XFT)
    friend void * qt_ft_font (const QFont *f);
#endif

#ifndef QT_NO_DATASTREAM
    friend Q_EXPORT QDataStream &operator<<( QDataStream &, const QFont & );
    friend Q_EXPORT QDataStream &operator>>( QDataStream &, QFont & );
#endif
    QFontData	 *d;				// internal font data
    static CharSet defaultCharSet;
};

inline bool QFont::bold() const
{ }

inline void QFont::setBold( bool enable )
{ }


/*****************************************************************************
  QFont stream functions
 *****************************************************************************/

#ifndef QT_NO_DATASTREAM
Q_EXPORT QDataStream &operator<<( QDataStream &, const QFont & );
Q_EXPORT QDataStream &operator>>( QDataStream &, QFont & );
#endif

#endif // QFONT_H
