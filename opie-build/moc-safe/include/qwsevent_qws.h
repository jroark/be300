/****************************************************************************
** $Id: qt/src/kernel/qwsevent_qws.h   2.3.10   edited 2005-01-24 $
**
** Declaration of Qt/Embedded events
**
** Created : 000101
**
** Copyright (C) 2000-2002 Trolltech AS.  All rights reserved.
**
** This file is part of the kernel module of the Qt GUI Toolkit.
**
** This file may be distributed and/or modified under the terms of the
** GNU General Public License version 2 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.
**
** Licensees holding valid Qt Enterprise Edition or Qt Professional Edition
** licenses for Qt/Embedded may use this file in accordance with the
** Qt Embedded Commercial License Agreement provided with the Software.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
** See http://www.trolltech.com/pricing.html or email sales@trolltech.com for
**   information about Qt Commercial License Agreements.
** See http://www.trolltech.com/gpl/ for GPL licensing information.
**
** Contact info@trolltech.com if any conditions of this licensing are
** not clear to you.
**
**********************************************************************/

#ifndef QWSEVENT_H
#define QWSEVENT_H

#ifndef QT_H
#include "qwsutils_qws.h"
#include "qwscommand_qws.h" //QWSProtocolItem lives there, for now
#endif // QT_H

struct QWSMouseEvent;

struct Q_EXPORT QWSEvent : QWSProtocolItem { };


//All events must start with windowID

struct Q_EXPORT QWSConnectedEvent : QWSEvent { };

struct Q_EXPORT QWSMaxWindowRectEvent : QWSEvent { };

struct Q_EXPORT QWSMouseEvent : QWSEvent { };

struct Q_EXPORT QWSFocusEvent : QWSEvent { };

struct Q_EXPORT QWSKeyEvent: QWSEvent { };


struct Q_EXPORT QWSCreationEvent : QWSEvent { };

#ifndef QT_NO_QWS_PROPERTIES
struct Q_EXPORT QWSPropertyNotifyEvent : QWSEvent { };
#endif

struct Q_EXPORT QWSSelectionClearEvent : QWSEvent { };

struct Q_EXPORT QWSSelectionRequestEvent : QWSEvent { };

struct Q_EXPORT QWSSelectionNotifyEvent : QWSEvent { };

//complex events:

struct Q_EXPORT QWSRegionModifiedEvent : QWSEvent { };
#ifndef QT_NO_QWS_PROPERTIES
struct Q_EXPORT QWSPropertyReplyEvent : QWSEvent { };
#endif //QT_NO_QWS_PROPERTIES

#ifndef QT_NO_COP
struct Q_EXPORT QWSQCopMessageEvent : QWSEvent { };

#endif

struct Q_EXPORT QWSWindowOperationEvent : QWSEvent { };

#ifndef QT_NO_QWS_IM
struct Q_EXPORT QWSIMEvent : QWSEvent { };
#endif
#endif
