/****************************************************************************
** $Id: qt/src/kernel/qwscommand_qws.h   2.3.10   edited 2005-01-24 $
**
** Implementation of Qt/FB central server
**
** Created : 991025
**
** Copyright (C) 1992-2000 Trolltech AS.  All rights reserved.
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

#ifndef QWSCOMMAND_H
#define QWSCOMMAND_H

#ifndef QT_H
#include "qwsutils_qws.h"
#include "qfont.h"
/* moc-stub */
#endif // QT_H

#define QTE_PIPE "QtEmbedded-%1"

/*********************************************************************
 *
 * Functions to read/write commands on/from a socket
 *
 *********************************************************************/
#ifndef QT_NO_QWS_MULTIPROCESS
void Q_EXPORT qws_write_command( QWSSocket *socket, int type, char *simpleData, int simpleLen, char *rawData, int rawLen );
bool Q_EXPORT qws_read_command( QWSSocket *socket, char *&simpleData, int &simpleLen, char *&rawData, int &rawLen, int &bytesRead );
#endif
/*********************************************************************
 *
 * QWSCommand base class - only use derived classes from that
 *
 *********************************************************************/


struct Q_EXPORT QWSProtocolItem
{ };


struct QWSCommand : QWSProtocolItem
{ };

/*********************************************************************
 *
 * Commands
 *
 *********************************************************************/

struct QWSIdentifyCommand : public QWSCommand
{ };

struct QWSCreateCommand : public QWSCommand
{ };

struct QWSRegionNameCommand : public QWSCommand
{ };

struct QWSRegionCommand : public QWSCommand
{ };

struct QWSRegionMoveCommand : public QWSCommand
{ };

struct QWSRegionDestroyCommand : public QWSCommand
{ };

struct QWSRequestFocusCommand : public QWSCommand
{ };

struct QWSChangeAltitudeCommand : public QWSCommand
{ };


struct QWSAddPropertyCommand : public QWSCommand
{ };

struct QWSSetPropertyCommand : public QWSCommand
{ };

struct QWSRemovePropertyCommand : public QWSCommand
{ };

struct QWSGetPropertyCommand : public QWSCommand
{ };

struct QWSSetSelectionOwnerCommand : public QWSCommand
{ };

struct QWSConvertSelectionCommand : public QWSCommand
{ };

struct QWSDefineCursorCommand : public QWSCommand
{ };

struct QWSSelectCursorCommand : public QWSCommand
{ };

struct QWSMoveCursorCommand : public QWSCommand
{ };

struct QWSGrabMouseCommand : public QWSCommand
{ };

struct QWSGrabKeyboardCommand : public QWSCommand
{ };

#ifndef QT_NO_SOUND
struct QWSPlaySoundCommand : public QWSCommand
{ };
#endif


#ifndef QT_NO_COP
struct QWSQCopRegisterChannelCommand : public QWSCommand
{ };

struct QWSQCopSendCommand : public QWSCommand
{ };

#endif

#ifndef QT_NO_QWS_IM
struct QWSSetIMInfoCommand : public QWSCommand
{ };

struct QWSIMMouseCommand : public QWSCommand
{ };

struct QWSResetIMCommand : public QWSCommand
{ };

struct QWSSetIMFontCommand : public QWSCommand
{ };



#endif

#endif // QWSCOMMAND_H
