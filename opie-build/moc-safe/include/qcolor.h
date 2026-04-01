/****************************************************************************
** $Id: qt/src/kernel/qcolor.h   2.3.10   edited 2005-01-24 $
**
** Definition of QColor class
**
** Created : 940112
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

#ifndef QCOLOR_H
#define QCOLOR_H

#ifndef QT_H
#include "qwindowdefs.h"
#endif // QT_H

const QRgb  RGB_DIRTY	= 0x80000000;		// flags unset color
const QRgb  RGB_INVALID = 0x40000000;		// flags invalid color
const QRgb  RGB_DIRECT	= 0x20000000;		// flags directly set pixel
const QRgb  RGB_MASK	= 0x00ffffff;		// masks RGB values


Q_EXPORT inline int qRed( QRgb rgb )		// get red part of RGB
{ }

Q_EXPORT inline int qGreen( QRgb rgb )		// get green part of RGB
{ }

Q_EXPORT inline int qBlue( QRgb rgb )		// get blue part of RGB
{ }

Q_EXPORT inline int qAlpha( QRgb rgb )		// get alpha part of RGBA
{ }

Q_EXPORT inline QRgb qRgb( int r, int g, int b )// set RGB value
{ }

Q_EXPORT inline QRgb qRgba( int r, int g, int b, int a )// set RGBA value
{ }

Q_EXPORT inline int qGray( int r, int g, int b )// convert R,G,B to gray 0..255
{ }

Q_EXPORT inline int qGray( QRgb rgb )		// convert RGB to gray 0..255
{ }


class Q_EXPORT QColor
{
public:
    enum Spec { };

    QColor();
    QColor( int r, int g, int b );
    QColor( int x, int y, int z, Spec );
    QColor( QRgb rgb, uint pixel=0xffffffff);
    QColor( const QString& name );
    QColor( const char *name );
    QColor( const QColor & );
    QColor &operator=( const QColor & );

    bool   isValid() const;
    bool   isDirty() const;

    QString name() const;
    void   setNamedColor( const QString& name );

    void   rgb( int *r, int *g, int *b ) const;
    QRgb   rgb()    const;
    void   setRgb( int r, int g, int b );
    void   setRgb( QRgb rgb );

    int	   red()    const;
    int	   green()  const;
    int	   blue()   const;

    void   hsv( int *h, int *s, int *v ) const;
    void   getHsv( int &h, int &s, int &v ) const;
    void   setHsv( int h, int s, int v );

    QColor light( int f = 150 ) const;
    QColor dark( int f = 200 )	const;

    bool   operator==( const QColor &c ) const;
    bool   operator!=( const QColor &c ) const;

    static bool lazyAlloc();
    static void setLazyAlloc( bool );
    uint   alloc();
    uint   pixel()  const;

    static int  maxColors();
    static int  numBitPlanes();

    static int  enterAllocContext();
    static void leaveAllocContext();
    static int  currentAllocContext();
    static void destroyAllocContext( int );

#if defined(_WS_WIN_)
    static HPALETTE hPal()  { }
    static uint	realizePal( QWidget * );
#endif

    static void initialize();
    static void cleanup();

private:
    void   setSystemNamedColor( const QString& name );
    static void initGlobalColors();
    static QColor* globalColors();
    static bool color_init;
    static bool globals_init;
    static bool lazy_alloc;
#if defined(_WS_WIN_)
    static HPALETTE hpal;
#endif
    uint   pix;
    QRgb   rgbVal;
};


inline QColor::QColor()
{ }

inline QColor::QColor( int r, int g, int b )
{ }

inline bool QColor::isValid() const
{ }

inline bool QColor::isDirty() const
{ }

inline QRgb QColor::rgb() const
{ }

inline int QColor::red() const
{ }

inline int QColor::green() const
{ }

inline int QColor::blue() const
{ }

inline uint QColor::pixel() const
{ }

inline bool QColor::lazyAlloc()
{ }


inline bool QColor::operator==( const QColor &c ) const
{ }

inline bool QColor::operator!=( const QColor &c ) const
{ }


/*****************************************************************************
  QColor stream functions
 *****************************************************************************/

#ifndef QT_NO_DATASTREAM
Q_EXPORT QDataStream &operator<<( QDataStream &, const QColor & );
Q_EXPORT QDataStream &operator>>( QDataStream &, QColor & );
#endif

#endif // QCOLOR_H
