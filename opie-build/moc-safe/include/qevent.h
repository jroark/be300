/****************************************************************************
** $Id: qt/src/kernel/qevent.h   2.3.10   edited 2005-01-24 $
**
** Definition of event classes
**
** Created : 931029
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

#ifndef QEVENT_H
#define QEVENT_H

#ifndef QT_H
#include "qwindowdefs.h"
#include "qregion.h"
#include "qnamespace.h"
#include "qmime.h"
#endif // QT_H


class Q_EXPORT QEvent: public Qt		// event base class
{
public:
    enum Type { };

    QEvent( Type type ) : t(type), posted(FALSE) { }
    virtual ~QEvent();
    Type  type() const	{ }
protected:
    Type  t;
private:
    bool  posted;
#if defined(_CC_MSVC_)
    friend class QEvent;
#endif

    friend class QApplication;
    friend class QBaseApplication;
};


class Q_EXPORT QTimerEvent : public QEvent
{
public:
    QTimerEvent( int timerId )
	: QEvent(Timer), id(timerId) { }
    int	  timerId()	const	{ }
protected:
    int	  id;
};


class Q_EXPORT QMouseEvent : public QEvent
{
public:
    QMouseEvent( Type type, const QPoint &pos, int button, int state );

    QMouseEvent( Type type, const QPoint &pos, const QPoint&globalPos,
		 int button, int state )
	: QEvent(type), p(pos), g(globalPos), b(button),s((ushort)state) { };

    const QPoint &pos() const	{ }
    const QPoint &globalPos() const { }
    int	   x()		const	{ }
    int	   y()		const	{ }
    int	   globalX()	const	{ }
    int	   globalY()	const	{ }
    ButtonState button() const	{ }
    ButtonState state()	const	{ }
    ButtonState stateAfter() const;
protected:
    QPoint p;
    QPoint g;
    int	   b; // ### Make ushort in 3.0? Here it's an int...
    ushort s; // ### ...and here an ushort. But both are ButtonState!
};



class Q_EXPORT QWheelEvent : public QEvent
{
public:
    QWheelEvent( const QPoint &pos, int delta, int state );
    QWheelEvent( const QPoint &pos, const QPoint& globalPos, int delta, int state )
	: QEvent(Wheel), p(pos), g(globalPos), d(delta), s((ushort)state),
	  accpt(TRUE) { }
    int	   delta()	const	{ }
    const QPoint &pos() const	{ }
    const QPoint &globalPos() const	{ }
    int	   x()		const	{ }
    int	   y()		const	{ }
    int	   globalX()	const	{ }
    int	   globalY()	const	{ }
    ButtonState state()	const	{ }
    bool   isAccepted() const	{ }
    void   accept()		{ }
    void   ignore()		{ }
protected:
    QPoint p;
    QPoint g;
    int d;
    ushort s;
    bool   accpt;
};


class Q_EXPORT QKeyEvent : public QEvent
{
public:
    QKeyEvent( Type type, int key, int ascii, int state,
		const QString& text=QString::null, bool autorep=FALSE, ushort count=1 )
	: QEvent(type), txt(text), k((ushort)key), s((ushort)state),
	    a((uchar)ascii), accpt(TRUE), autor(autorep), c(count) { }
    int	   key()	const	{ }
    int	   ascii()	const	{ }
    ButtonState state()	const	{ }
    ButtonState stateAfter() const;
    bool   isAccepted() const	{ }
    QString text()      const   { }
    bool   isAutoRepeat() const	{ }
    int   count() const	{ }
    void   accept()		{ }
    void   ignore()		{ }

protected:
    QString txt;
    ushort k, s;
    uchar  a;
    uint   accpt:1;
    uint   autor:1;
    ushort c;
};


class Q_EXPORT QFocusEvent : public QEvent
{
public:

    QFocusEvent( Type type )
	: QEvent(type) { }

    bool   gotFocus()	const { }
    bool   lostFocus()	const { }

    enum Reason { };
    static Reason reason();
    static void setReason( Reason reason );
    static void resetReason();

private:
    static Reason m_reason;
    static Reason prev_reason;
};


class Q_EXPORT QPaintEvent : public QEvent
{
public:
    QPaintEvent( const QRegion& paintRegion, bool erased = TRUE)
	: QEvent(Paint),
	  rec(paintRegion.boundingRect()),
	  reg(paintRegion),
	  erase(erased){ }
    QPaintEvent( const QRect &paintRect, bool erased = TRUE )
	: QEvent(Paint),
	  rec(paintRect),
	  reg(paintRect),
	  erase(erased){ }
    const QRect &rect() const	  { }
    const QRegion &region() const { }
    bool erased() const { }
protected:
    friend class QApplication;
    friend class QBaseApplication;
    QRect rec;
    QRegion reg;
    bool erase;
};


class Q_EXPORT QMoveEvent : public QEvent
{
public:
    QMoveEvent( const QPoint &pos, const QPoint &oldPos )
	: QEvent(Move), p(pos), oldp(oldPos) { }
    const QPoint &pos()	  const { }
    const QPoint &oldPos()const { }
protected:
    QPoint p, oldp;
    friend class QApplication;
    friend class QBaseApplication;
};


class Q_EXPORT QResizeEvent : public QEvent
{
public:
    QResizeEvent( const QSize &size, const QSize &oldSize )
	: QEvent(Resize), s(size), olds(oldSize) { }
    const QSize &size()	  const { }
    const QSize &oldSize()const { }
protected:
    QSize s, olds;
    friend class QApplication;
    friend class QBaseApplication;
};


class Q_EXPORT QCloseEvent : public QEvent
{
public:
    QCloseEvent()
	: QEvent(Close), accpt(FALSE) { }
    bool   isAccepted() const	{ }
    void   accept()		{ }
    void   ignore()		{ }
protected:
    bool   accpt;
};


class Q_EXPORT QShowEvent : public QEvent
{
public:
    QShowEvent(bool spontaneous)
	: QEvent(Show), spont(spontaneous) { }
    bool spontaneous() const { }
protected:
    bool spont;
};


class Q_EXPORT QHideEvent : public QEvent
{
public:
    QHideEvent(bool spontaneous)
	: QEvent(Hide), spont(spontaneous) { }
    bool spontaneous() const { }
protected:
    bool spont;
};


class Q_EXPORT QIMEvent : public QEvent
{
public:
    QIMEvent( Type type, const QString &text, int cursorPosition )
	: QEvent(type), txt(text), cpos(cursorPosition), a(FALSE) { }
    const QString &text() const { }
    int cursorPos() const { }
    bool isAccepted() const { }
    void accept() { }
    void ignore() { }
    int selectionLength() const;

private:
    QString txt;
    int cpos;
    bool a;
};

class Q_EXPORT QIMComposeEvent : public QIMEvent
{
public:
    QIMComposeEvent( Type type, const QString &text, int cursorPosition,
		     int selLength )
	: QIMEvent( type, text, cursorPosition ), selLen( selLength ) { }

private:
    int selLen;

    friend class QIMEvent;
};

inline int QIMEvent::selectionLength() const
{ }




#ifndef QT_NO_DRAGANDDROP

// This class is rather closed at the moment.  If you need to create your
// own DND event objects, write to qt-bugs@trolltech.com and we'll try to
// find a way to extend it so it covers your needs.

class Q_EXPORT QDropEvent : public QEvent, public QMimeSource
{
public:
    QDropEvent( const QPoint& pos, Type typ=Drop )
	: QEvent(typ), p(pos),
	  act(0), accpt(0), accptact(0), resv(0),
	  d(0)
	{ }
    const QPoint &pos() const	{ }
    bool isAccepted() const	{ }
    void accept(bool y=TRUE)	{ }
    void ignore()		{ }

    bool isActionAccepted() const { }
    void acceptAction(bool y=TRUE) { }
    enum Action { };
    void setAction( Action a ) { }
    Action action() const { }

    QWidget* source() const;
    const char* format( int n = 0 ) const;
    QByteArray encodedData( const char* ) const;
    bool provides( const char* ) const;

    QByteArray data(const char* f) const { }

    void setPoint( const QPoint& np ) { }

protected:
    QPoint p;
    uint act:8;
    uint accpt:1;
    uint accptact:1;
    uint resv:5;
    void * d;
};



class Q_EXPORT QDragMoveEvent : public QDropEvent
{
public:
    QDragMoveEvent( const QPoint& pos, Type typ=DragMove )
	: QDropEvent(pos,typ),
	  rect( pos, QSize( 1, 1 ) ) { }
    QRect answerRect() const { }
    void accept( bool y=TRUE ) { }
    void accept( const QRect & r) { }
    void ignore( const QRect & r) { }
    void ignore()		{ }

protected:
    QRect rect;
};


class Q_EXPORT QDragEnterEvent : public QDragMoveEvent
{
public:
    QDragEnterEvent( const QPoint& pos ) :
	QDragMoveEvent(pos, DragEnter) { }
};


/* An internal class */
class Q_EXPORT QDragResponseEvent : public QEvent
{
public:
    QDragResponseEvent( bool accepted )
	: QEvent(DragResponse), a(accepted) { }
    bool   dragAccepted() const	{ }
protected:
    bool a;
};


class Q_EXPORT QDragLeaveEvent : public QEvent
{
public:
    QDragLeaveEvent()
	: QEvent(DragLeave) { }
};

#endif // QT_NO_DRAGANDDROP

class Q_EXPORT QChildEvent : public QEvent
{
public:
    QChildEvent( Type type, QObject *child )
	: QEvent(type), c(child) { }
    QObject *child() const	{ }
    bool inserted() const { }
    bool removed() const { }
protected:
    QObject *c;
};


class Q_EXPORT QCustomEvent : public QEvent
{
public:
    QCustomEvent( int type );
    QCustomEvent( Type type, void *data )
	: QEvent(type), d(data) { };
    void       *data()	const	{ }
    void	setData( void* data )	{ }
private:
    void       *d;
};

#endif // QEVENT_H
