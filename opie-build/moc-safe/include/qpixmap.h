/****************************************************************************
** $Id: qt/src/kernel/qpixmap.h   2.3.10   edited 2005-01-24 $
**
** Definition of QPixmap class
**
** Created : 940501
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

#ifndef QPIXMAP_H
#define QPIXMAP_H

#ifndef QT_H
#include "qpaintdevice.h"
#include "qcolor.h"
#include "qstring.h"
#include "qnamespace.h"
#endif // QT_H

class QGfx;

#if defined(_WS_WIN_)
// Internal pixmap memory optimization class for Windows 9x
class QMultiCellPixmap;
#endif


class Q_EXPORT QPixmap : public QPaintDevice, public Qt
{
public:
    enum ColorMode { };
    enum Optimization { };

    QPixmap();
    QPixmap( int w, int h,  int depth = -1, Optimization = DefaultOptim );
    QPixmap( const QSize &, int depth = -1, Optimization = DefaultOptim );
    QPixmap( const QString& fileName, const char *format=0,
	     ColorMode mode=Auto );
    QPixmap( const QString& fileName, const char *format,
	     int conversion_flags );
    QPixmap( const char *xpm[] );
    QPixmap( const QByteArray &data );
    QPixmap( const QPixmap & );
   ~QPixmap();

    QPixmap    &operator=( const QPixmap & );
    QPixmap    &operator=( const QImage	 & );

    bool	isNull()	const;

    int		width()		const { }
    int		height()	const { }
    QSize	size()		const { }
    QRect	rect()		const { }
    int		depth()		const { }
    static int	defaultDepth();

    void	fill( const QColor &fillColor = Qt::white );
    void	fill( const QWidget *, int xofs, int yofs );
    void	fill( const QWidget *, const QPoint &ofs );
    void	resize( int width, int height );
    void	resize( const QSize & );

    const QBitmap *mask() const;
    void	setMask( const QBitmap & );
    bool	selfMask() const;
    QBitmap	createHeuristicMask( bool clipTight = TRUE ) const;

    static  QPixmap grabWindow( WId, int x=0, int y=0, int w=-1, int h=-1 );
    static  QPixmap grabWidget( QWidget * widget,
				int x=0, int y=0, int w=-1, int h=-1 );

#ifndef QT_NO_PIXMAP_TRANSFORMATION
    QPixmap	    xForm( const QWMatrix & ) const;
#endif
#ifndef QT_NO_WMATRIX
    static QWMatrix trueMatrix( const QWMatrix &, int w, int h );
#endif

    QImage	convertToImage() const;
    bool	convertFromImage( const QImage &, ColorMode mode=Auto );
    bool	convertFromImage( const QImage &, int conversion_flags );

    static const char* imageFormat( const QString &fileName );
    bool	load( const QString& fileName, const char *format=0,
		      ColorMode mode=Auto );
    bool	load( const QString& fileName, const char *format,
		      int conversion_flags );
    bool	loadFromData( const uchar *buf, uint len,
			      const char* format=0,
			      ColorMode mode=Auto );
    bool	loadFromData( const uchar *buf, uint len,
			      const char* format,
			      int conversion_flags );
    bool	loadFromData( const QByteArray &data,
			      const char* format=0,
			      int conversion_flags=0 );
    bool	save( const QString& fileName, const char* format ) const; // ### remove 3.0
    bool	save( const QString& fileName, const char* format,
		      int quality ) const; // ### change to quality=-1 in 3.0

#if defined(_WS_WIN_)
    HBITMAP	hbm()		const;
#endif

    int		serialNumber()	const;

    Optimization	optimization() const;
    void		setOptimization( Optimization );
    static Optimization defaultOptimization();
    static void		setDefaultOptimization( Optimization );

    virtual void detach();

    bool	isQBitmap() const;

#if defined(_WS_WIN_)
    // These functions are internal and used by Windows 9x only
    bool	isMultiCellPixmap() const;
    HDC		multiCellHandle() const;
    HBITMAP	multiCellBitmap() const;
    int		multiCellOffset() const;
    int		allocCell();
    void	freeCell( bool = FALSE );
#endif

#if defined(_WS_QWS_)
    virtual QGfx * graphicsContext(bool clip_children=TRUE) const;
    virtual unsigned char * scanLine(int) const;
    virtual int bytesPerLine() const;
    QRgb * clut() const;
    int numCols() const;
    bool hasAlpha() const { }
#else
    bool hasAlpha() const { }
#endif

#if defined(Q_FULL_TEMPLATE_INSTANTIATION)
    bool operator==( const QPixmap& ) const { }
#endif

protected:
    QPixmap( int w, int h, const uchar *data, bool isXbitmap );
    int metric( int ) const;

#if defined(_WS_WIN_)
    struct QMCPI { };
#endif

    struct QPixmapData : public QShared { } *data;

private:
    QPixmap( int w, int h, int depth, bool, Optimization );
    void	init( int, int, int, bool, Optimization );
    void	deref();
    QPixmap	copy( bool ignoreMask = FALSE ) const;
    static Optimization defOptim;
    friend Q_EXPORT void bitBlt( QPaintDevice *, int, int,
				 const QPaintDevice *,
				 int, int, int, int, RasterOp, bool );
    friend Q_EXPORT void bitBlt( QPaintDevice *, int, int,
				 const QImage* src,
				 int, int, int, int, int conversion_flags );
    friend class QBitmap;
    friend class QPaintDevice;
    friend class QPainter;
    friend class QSharedMemoryManager;
};


inline bool QPixmap::isNull() const
{ }

inline void QPixmap::fill( const QWidget *w, const QPoint &ofs )
{ }

inline void QPixmap::resize( const QSize &s )
{ }

inline const QBitmap *QPixmap::mask() const
{ }

inline bool QPixmap::selfMask() const
{ }

#if defined(_WS_WIN_)
inline HBITMAP QPixmap::hbm() const
{ }
#endif

inline int QPixmap::serialNumber() const
{ }

inline QPixmap::Optimization QPixmap::optimization() const
{ }

inline bool QPixmap::isQBitmap() const
{ }

#if defined(_WS_WIN_)
inline bool QPixmap::isMultiCellPixmap() const
{ }
#endif


/*****************************************************************************
  QPixmap stream functions
 *****************************************************************************/

#ifndef QT_NO_DATASTREAM
Q_EXPORT QDataStream &operator<<( QDataStream &, const QPixmap & );
Q_EXPORT QDataStream &operator>>( QDataStream &, QPixmap & );
#endif

#endif // QPIXMAP_H
