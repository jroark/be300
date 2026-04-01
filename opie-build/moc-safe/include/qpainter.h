/****************************************************************************
** $Id: qt/src/kernel/qpainter.h   2.3.10   edited 2005-01-24 $
**
** Definition of QPainter class
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

#ifndef QPAINTER_H
#define QPAINTER_H


#ifndef QT_H
#include "qpaintdevice.h"
#include "qcolor.h"
#include "qfontmetrics.h"
#include "qfontinfo.h"
#include "qregion.h"
#include "qpen.h"
#include "qbrush.h"
#include "qpointarray.h"
#include "qwmatrix.h"
#endif // QT_H

class QGfx;
class QTextCodec;


class Q_EXPORT QPainter : public Qt
{
public:
    QPainter();
    QPainter( const QPaintDevice * );
    QPainter( const QPaintDevice *, const QWidget * );
   ~QPainter();

    bool	begin( const QPaintDevice * );
    bool	begin( const QPaintDevice *, const QWidget * );
    bool	end();
    QPaintDevice *device() const;

#ifdef _WS_QWS_
    QGfx * internalGfx();
#endif

    static void redirect( QPaintDevice *pdev, QPaintDevice *replacement );

    bool	isActive() const;

    void	flush();
    void	save();
    void	restore();

  // Drawing tools

    QFontMetrics fontMetrics()	const;
    QFontInfo	 fontInfo()	const;

    const QFont &font()		const;
    void	setFont( const QFont & );
    const QPen &pen()		const;
    void	setPen( const QPen & );
    void	setPen( PenStyle );
    void	setPen( const QColor & );
    const QBrush &brush()	const;
    void	setBrush( const QBrush & );
    void	setBrush( BrushStyle );
    void	setBrush( const QColor & );
    QPoint	pos() const;

  // Drawing attributes/modes

    const QColor &backgroundColor() const;
    void	setBackgroundColor( const QColor & );
    BGMode	backgroundMode() const;
    void	setBackgroundMode( BGMode );
    RasterOp	rasterOp()	const;
    void	setRasterOp( RasterOp );
    const QPoint &brushOrigin() const;
    void	setBrushOrigin( int x, int y );
    void	setBrushOrigin( const QPoint & );

  // Scaling and transformations

//    PaintUnit unit()	       const;		// get set painter unit
//    void	setUnit( PaintUnit );		// NOT IMPLEMENTED!!!

    bool	hasViewXForm() const;
    bool	hasWorldXForm() const;

#ifndef QT_NO_TRANSFORMATIONS
    void	setViewXForm( bool );		// set xform on/off
    QRect	window()       const;		// get window
    void	setWindow( const QRect & );	// set window
    void	setWindow( int x, int y, int w, int h );
    QRect	viewport()   const;		// get viewport
    void	setViewport( const QRect & );	// set viewport
    void	setViewport( int x, int y, int w, int h );

    void	setWorldXForm( bool );		// set world xform on/off
    const QWMatrix &worldMatrix() const;	// get/set world xform matrix
    void	setWorldMatrix( const QWMatrix &, bool combine=FALSE );

    void	saveWorldMatrix();
    void	restoreWorldMatrix();

    void	scale( double sx, double sy );
    void	shear( double sh, double sv );
    void	rotate( double a );
#endif
    void	translate( double dx, double dy );
    void	resetXForm();

    QPoint	xForm( const QPoint & ) const;	// map virtual -> device
    QRect	xForm( const QRect & )	const;
    QPointArray xForm( const QPointArray & ) const;
    QPointArray xForm( const QPointArray &, int index, int npoints ) const;
    QPoint	xFormDev( const QPoint & ) const; // map device -> virtual
    QRect	xFormDev( const QRect & )  const;
    QPointArray xFormDev( const QPointArray & ) const;
    QPointArray xFormDev( const QPointArray &, int index, int npoints ) const;

  // Clipping

    void	setClipping( bool );		// set clipping on/off
    bool	hasClipping() const;
    const QRegion &clipRegion() const;
    void	setClipRect( const QRect & );	// set clip rectangle
    void	setClipRect( int x, int y, int w, int h );
    void	setClipRegion( const QRegion &);// set clip region

  // Graphics drawing functions

    void	drawPoint( int x, int y );
    void	drawPoint( const QPoint & );
    void	drawPoints( const QPointArray& a,
			    int index=0, int npoints=-1 );
    void	moveTo( int x, int y );
    void	moveTo( const QPoint & );
    void	lineTo( int x, int y );
    void	lineTo( const QPoint & );
    void	drawLine( int x1, int y1, int x2, int y2 );
    void	drawLine( const QPoint &, const QPoint & );
    void	drawRect( int x, int y, int w, int h );
    void	drawRect( const QRect & );
    void	drawWinFocusRect( int x, int y, int w, int h );
    void	drawWinFocusRect( int x, int y, int w, int h,
				  const QColor &bgColor );
    void	drawWinFocusRect( const QRect & );
    void	drawWinFocusRect( const QRect &,
				  const QColor &bgColor );
    void	drawRoundRect( int x, int y, int w, int h, int, int );
    void	drawRoundRect( const QRect &, int, int );
    void	drawRoundRect( int x, int y, int w, int h );
    void	drawRoundRect( const QRect & );
    void	drawEllipse( int x, int y, int w, int h );
    void	drawEllipse( const QRect & );
    void	drawArc( int x, int y, int w, int h, int a, int alen );
    void	drawArc( const QRect &, int a, int alen );
    void	drawPie( int x, int y, int w, int h, int a, int alen );
    void	drawPie( const QRect &, int a, int alen );
    void	drawChord( int x, int y, int w, int h, int a, int alen );
    void	drawChord( const QRect &, int a, int alen );
    void	drawLineSegments( const QPointArray &,
				  int index=0, int nlines=-1 );
    void	drawPolyline( const QPointArray &,
			      int index=0, int npoints=-1 );
    void	drawPolygon( const QPointArray &, bool winding=FALSE,
			     int index=0, int npoints=-1 );
    void	drawQuadBezier( const QPointArray &, int index=0 );
    void	drawPixmap( int x, int y, const QPixmap &,
			    int sx=0, int sy=0, int sw=-1, int sh=-1 );
    void	drawPixmap( const QPoint &, const QPixmap &,
			    const QRect &sr );
    void	drawPixmap( const QPoint &, const QPixmap & );
    void	drawImage( int x, int y, const QImage &,
			   int sx=0, int sy=0, int sw=-1, int sh=-1 );
    void	drawImage( const QPoint &, const QImage &, const QRect &sr );
    void	drawImage( const QPoint &, const QImage & );
    void	drawImage( int x, int y, const QImage &,
			   int sx, int sy, int sw, int sh, int conversion_flags );
    void	drawImage( const QPoint &, const QImage &, const QRect &sr, int conversion_flags );
    void	drawImage( const QPoint &, const QImage &, int conversion_flags );
    void	drawTiledPixmap( int x, int y, int w, int h, const QPixmap &,
				 int sx=0, int sy=0 );
    void	drawTiledPixmap( const QRect &, const QPixmap &,
				 const QPoint & );
    void	drawTiledPixmap( const QRect &, const QPixmap & );
#ifndef QT_NO_PICTURE
    void	drawPicture( const QPicture & );
#endif

    void	fillRect( int x, int y, int w, int h, const QBrush & );
    void	fillRect( const QRect &, const QBrush & );
    void	eraseRect( int x, int y, int w, int h );
    void	eraseRect( const QRect & );

  // Text drawing functions

    void	drawText( int x, int y, const QString &, int len = -1 );
    void	drawText( const QPoint &, const QString &, int len = -1 );
    void	drawText( int x, int y, int w, int h, int flags,
			  const QString&, int len = -1, QRect *br=0,
			  char **internal=0 );
    void	drawText( const QRect &, int flags,
			  const QString&, int len = -1, QRect *br=0,
			  char **internal=0 );

    //#####    void	drawText( const QPoint &, const QString &, int flags, int rotation = 0);

  // Text drawing functions

    QRect	boundingRect( int x, int y, int w, int h, int flags,
			      const QString&, int len = -1, char **intern=0 );
    QRect	boundingRect( const QRect &, int flags,
			      const QString&, int len = -1, char **intern=0 );

    int		tabStops() const;
    void	setTabStops( int );
    int	       *tabArray() const;
    void	setTabArray( int * );

    // Other functions

#if defined(_WS_WIN_)
    HDC		handle() const;
#elif defined(_WS_X11_)
    HANDLE	handle() const;
#endif


    static void initialize();
    static void cleanup();

private:
    void	init();
    void	updateFont();
    void	updatePen();
    void	updateBrush();
#ifndef QT_NO_TRANSFORMATIONS
    void	updateXForm();
    void	updateInvXForm();
#endif
    void	map( int, int, int *rx, int *ry ) const;
    void	map( int, int, int, int, int *, int *, int *, int * ) const;
    void	mapInv( int, int, int *, int * ) const;
    void	mapInv( int, int, int, int, int *, int *, int *, int * ) const;
    void	drawPolyInternal( const QPointArray &, bool close=TRUE );
    void	drawWinFocusRect( int x, int y, int w, int h, bool xorPaint,
				  const QColor &penColor );

    enum { };
    uint	flags;
    bool	testf( uint b ) const { }
    void	setf( uint b )	{ }
    void	setf( uint b, bool v );
    void	clearf( uint b )	{ }
    void	fix_neg_rect( int *x, int *y, int *w, int *h );

    QPaintDevice *pdev;
    QColor	bg_col;
    uchar	bg_mode;
    uchar	rop;
    uchar	pu;
    QPoint	bro;
    QFont	cfont;
    QPen	cpen;
    QBrush	cbrush;
    QRegion	crgn;
    int		tabstops;
    int	       *tabarray;
    int		tabarraylen;

    // Transformations
#ifndef QT_NO_TRANSFORMATIONS
    QCOORD	wx, wy, ww, wh;
    QCOORD	vx, vy, vw, vh;
    QWMatrix	wxmat;

    // Cached composition (and inverse) of transformations
    QWMatrix	xmat;
    QWMatrix	ixmat;



    double	m11() const { }
    double      m12() const { }
    double      m21() const { }
    double      m22() const { }
    double      dx() const { }
    double      dy() const { }
    double	im11() const { }
    double      im12() const { }
    double      im21() const { }
    double      im22() const { }
    double      idx() const { }
    double      idy() const { }

    int		txop;
    bool	txinv;

#else
    // even without transformations we still have translations
    int		xlatex;
    int		xlatey;
#endif

    void       *penRef;				// pen cache ref
    void       *brushRef;			// brush cache ref
    void       *ps_stack;
    void       *wm_stack;
    void	killPStack();

protected:
#if defined(_WS_WIN_)
    QT_WIN_PAINTER_MEMBERS
#elif defined(_WS_X11_)
    Display    *dpy;				// current display
    WId		hd;				// handle to drawable
    GC		gc;				// graphics context (standard)
    GC		gc_brush;			// graphics contect for brush
    QPoint	curPt;				// current point
#elif defined(_WS_MAC_)
    int penx;
    int peny;
    void * hd;
#elif defined(_WS_QWS_)
    QGfx * gfx;
    friend void qwsUpdateActivePainters();
#endif
    friend class QFontMetrics;
    friend class QFontInfo;
    friend void qt_format_text( const QFontMetrics& fm, int x, int y, int w, int h,
		     int tf, const QString& str, int len, QRect *brect,
		     int tabstops, int* tabarray, int tabarraylen,
		     char **internal, QPainter* painter );

private:	// Disabled copy constructor and operator=
#if defined(Q_DISABLE_COPY)
    QPainter( const QPainter & );
    QPainter &operator=( const QPainter & );
#endif
};


/*****************************************************************************
  QPainter member functions
 *****************************************************************************/

inline QPaintDevice *QPainter::device() const
{ }

inline bool QPainter::isActive() const
{ }

inline const QFont &QPainter::font() const
{ }

inline const QPen &QPainter::pen() const
{ }

inline const QBrush &QPainter::brush() const
{ }

/*
inline PaintUnit QPainter::unit() const
{ }
*/

inline const QColor &QPainter::backgroundColor() const
{ }

inline Qt::BGMode QPainter::backgroundMode() const
{ }

inline Qt::RasterOp QPainter::rasterOp() const
{ }

inline const QPoint &QPainter::brushOrigin() const
{ }

inline bool QPainter::hasViewXForm() const
{ }

inline bool QPainter::hasWorldXForm() const
{ }

inline bool QPainter::hasClipping() const
{ }

inline const QRegion &QPainter::clipRegion() const
{ }

inline int QPainter::tabStops() const
{ }

inline int *QPainter::tabArray() const
{ }

#if defined(_WS_WIN_)
inline HDC QPainter::handle() const
{ }
#elif defined(_WS_X11_)
inline HANDLE QPainter::handle() const
{ }
#endif

inline void QPainter::setBrushOrigin( const QPoint &p )
{ }

#ifndef QT_NO_TRANSFORMATIONS
inline void QPainter::setWindow( const QRect &r )
{ }

inline void QPainter::setViewport( const QRect &r )
{ }
#endif

inline void QPainter::setClipRect( int x, int y, int w, int h )
{ }

inline void QPainter::drawPoint( const QPoint &p )
{ }

inline void QPainter::moveTo( const QPoint &p )
{ }

inline void QPainter::lineTo( const QPoint &p )
{ }

inline void QPainter::drawLine( const QPoint &p1, const QPoint &p2 )
{ }

inline void QPainter::drawRect( const QRect &r )
{ }

inline void QPainter::drawWinFocusRect( const QRect &r )
{ }

inline void QPainter::drawWinFocusRect( const QRect &r,const QColor &penColor )
{ }

inline void QPainter::drawRoundRect( const QRect &r, int xRnd, int yRnd )
{ }

inline void QPainter::drawRoundRect( const QRect &r )
{ }

inline void QPainter::drawRoundRect( int x, int y, int w, int h )
{ }




inline void QPainter::drawEllipse( const QRect &r )
{ }

inline void QPainter::drawArc( const QRect &r, int a, int alen )
{ }

inline void QPainter::drawPie( const QRect &r, int a, int alen )
{ }

inline void QPainter::drawChord( const QRect &r, int a, int alen )
{ }

inline void QPainter::drawPixmap( const QPoint &p, const QPixmap &pm,
				  const QRect &sr )
{ }

inline void QPainter::drawImage( const QPoint &p, const QImage &pm,
				 const QRect &sr )
{ }

inline void QPainter::drawTiledPixmap( const QRect &r, const QPixmap &pm,
				       const QPoint &sp )
{ }

inline void QPainter::drawTiledPixmap( const QRect &r, const QPixmap &pm )
{ }

inline void QPainter::fillRect( const QRect &r, const QBrush &brush )
{ }

inline void QPainter::eraseRect( int x, int y, int w, int h )
{ }

inline void QPainter::eraseRect( const QRect &r )
{ }

inline void QPainter::drawText( const QPoint &p, const QString &s, int len )
{ }

inline void QPainter::drawText( const QRect &r, int tf,
				const QString& str, int len, QRect *br, char **i )
{ }

inline QRect QPainter::boundingRect( const QRect &r, int tf,
				     const QString& str, int len, char **i )
{ }

#if defined(_WS_WIN_)
inline void *QPainter::textMetric()
{ }
#endif

#if defined(_WS_QWS_)
inline QGfx * QPainter::internalGfx()
{ }
#endif

#endif // QPAINTER_H
