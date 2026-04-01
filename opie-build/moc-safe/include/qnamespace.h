/****************************************************************************
** $Id: qt/src/kernel/qnamespace.h   2.3.10   edited 2005-01-24 $
**
** Definition of Qt namespace (as class for compiler compatibility)
**
** Created : 980927
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

#ifndef QNAMESPACE_H
#define QNAMESPACE_H

#ifndef QT_H
#include "qglobal.h"
#endif // QT_H


class QColor;
class QCursor;


class Q_EXPORT Qt {
public:
    QT_STATIC_CONST QColor & color0;
    QT_STATIC_CONST QColor & color1;
    QT_STATIC_CONST QColor & black;
    QT_STATIC_CONST QColor & white;
    QT_STATIC_CONST QColor & darkGray;
    QT_STATIC_CONST QColor & gray;
    QT_STATIC_CONST QColor & lightGray;
    QT_STATIC_CONST QColor & red;
    QT_STATIC_CONST QColor & green;
    QT_STATIC_CONST QColor & blue;
    QT_STATIC_CONST QColor & cyan;
    QT_STATIC_CONST QColor & magenta;
    QT_STATIC_CONST QColor & yellow;
    QT_STATIC_CONST QColor & darkRed;
    QT_STATIC_CONST QColor & darkGreen;
    QT_STATIC_CONST QColor & darkBlue;
    QT_STATIC_CONST QColor & darkCyan;
    QT_STATIC_CONST QColor & darkMagenta;
    QT_STATIC_CONST QColor & darkYellow;

    // documented in qevent.cpp
    enum ButtonState { };

    // documented in qobject.cpp
    enum Orientation { };

    // Text formatting flags for QPainter::drawText and QLabel

    // documented in qpainter.cpp
    enum AlignmentFlags { };

    // QWidget state flags (internal, not documented but should be)
    enum WidgetState { };

    // Widget flags2
    typedef uint WFlags;

    // documented in qwidget.cpp
    enum WidgetFlags { };

    // Image conversion flags.  The unusual ordering is caused by
    // compatibility and default requirements.

    enum ImageConversionFlags { };

    enum BGMode	{ };

    enum PaintUnit { };

    enum GUIStyle { };

    enum Modifier { };

    enum Key { };

    enum ArrowType { };

    // documented in qpainter.cpp
    enum RasterOp { };

    // documented in qpainter.cpp
    enum PenStyle { };

    enum PenCapStyle { };

    enum PenJoinStyle { };

    enum BrushStyle { };

    enum WindowsVersion { };

    enum UIEffect { };


    // Global cursors

    QT_STATIC_CONST QCursor & arrowCursor;	// standard arrow cursor
    QT_STATIC_CONST QCursor & upArrowCursor;	// upwards arrow
    QT_STATIC_CONST QCursor & crossCursor;	// crosshair
    QT_STATIC_CONST QCursor & waitCursor;	// hourglass/watch
    QT_STATIC_CONST QCursor & ibeamCursor;	// ibeam/text entry
    QT_STATIC_CONST QCursor & sizeVerCursor;	// vertical resize
    QT_STATIC_CONST QCursor & sizeHorCursor;	// horizontal resize
    QT_STATIC_CONST QCursor & sizeBDiagCursor;	// diagonal resize (/)
    QT_STATIC_CONST QCursor & sizeFDiagCursor;	// diagonal resize (\)
    QT_STATIC_CONST QCursor & sizeAllCursor;	// all directions resize
    QT_STATIC_CONST QCursor & blankCursor;	// blank/invisible cursor
    QT_STATIC_CONST QCursor & splitVCursor;	// vertical bar with left-right
						// arrows
    QT_STATIC_CONST QCursor & splitHCursor;	// horizontal bar with up-down
						// arrows
    QT_STATIC_CONST QCursor & pointingHandCursor;	// pointing hand
    QT_STATIC_CONST QCursor & forbiddenCursor;	// forbidden cursor (slashed circle)


    enum TextFormat { };
};


class Q_EXPORT QInternal {
public:
    enum PaintDeviceFlags { };
};

#endif // QNAMESPACE_H
