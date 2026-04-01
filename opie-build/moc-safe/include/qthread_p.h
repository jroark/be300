/****************************************************************************
** $Id: qt/src/kernel/qthread_p.h   2.3.10   edited 2005-01-24 $
**
** QThread class for Unix
**
** Created : 20001309
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

#ifndef QTHREAD_P_H
#define QTHREAD_P_H

#define gettimeofday __hide_gettimeofday
/* moc-stub */

/* moc-stub */
/* moc-stub */
#ifdef QWS
/* moc-stub */
#else
/* moc-stub */
#endif
/* moc-stub */
/* moc-stub */

#undef	gettimeofday
extern "C" int gettimeofday( struct timeval *, struct timezone * );


// Thread definitions for UNIX platforms

#if defined(_OS_LINUX_)
#ifdef _XOPEN_SOURCE
#undef  _XOPEN_SOURCE
#endif
#  define _XOPEN_SOURCE 500
#  if (__GLIBC__ == 2) && (__GLIBC_MINOR__ == 0)
// Linux with glibc 2.0.x - POSIX 1003.4a thread implementation
#    define Q_HAS_RECURSIVE_MUTEX
#    define Q_USE_PTHREAD_MUTEX_SETKIND
#    define Q_NORMAL_MUTEX_TYPE PTHREAD_MUTEX_ERRORCHECK_NP
#    define Q_RECURSIVE_MUTEX_TYPE PTHREAD_MUTEX_RECURSIVE_NP
#  else
// Linux with glibc 2.1.x - POSIX 1003.1c thread implementation
#    define Q_HAS_RECURSIVE_MUTEX
#    undef  Q_USE_PTHREAD_MUTEX_SETKIND
#    define Q_NORMAL_MUTEX_TYPE PTHREAD_MUTEX_DEFAULT
#    define Q_RECURSIVE_MUTEX_TYPE PTHREAD_MUTEX_RECURSIVE
#  endif
#elif defined(_OS_OSF_)
// Tru64 4.0 and later - POSIX 1003.1c implementation
#  define Q_HAS_RECURSIVE_MUTEX
#  undef  Q_USE_PTHREAD_MUTEX_SETKIND
#  define Q_NORMAL_MUTEX_TYPE PTHREAD_MUTEX_ERRORCHECK
#  define Q_RECURSIVE_MUTEX_TYPE PTHREAD_MUTEX_RECURSIVE
#elif defined(_OS_AIX_)
// AIX 4.3.x
#  define Q_HAS_RECURSIVE_MUTEX
#  undef  Q_USE_PTHREAD_MUTEX_SETKIND
#  define Q_NORMAL_MUTEX_TYPE PTHREAD_MUTEX_ERRORCHECK
#  define Q_RECURSIVE_MUTEX_TYPE PTHREAD_MUTEX_RECURSIVE
#elif defined(_OS_HPUX_)
// We only support HP/UX 11.x
#  define Q_HAS_RECURSIVE_MUTEX
#  undef  Q_USE_PTHREAD_MUTEX_SETKIND
#  define Q_NORMAL_MUTEX_TYPE PTHREAD_MUTEX_ERRORCHECK
#  define Q_RECURSIVE_MUTEX_TYPE PTHREAD_MUTEX_RECURSIVE
#elif defined (_OS_FREEBSD_) || defined(_OS_OPENBSD_)
// FreeBSD and OpenBSD use the same user-space thread implementation
#  define Q_HAS_RECURSIVE_MUTEX
#  undef  Q_USE_PTHREAD_MUTEX_SETKIND
#  define Q_NORMAL_MUTEX_TYPE PTHREAD_MUTEX_ERRORCHECK
#  define Q_RECURSIVE_MUTEX_TYPE PTHREAD_MUTEX_RECURSIVE
#elif defined(_OS_SOLARIS_)
// Solaris 2.7 and later - we use the native Solaris threads implementation
#  undef  Q_HAS_RECURSIVE_MUTEX
#  undef  Q_USE_PTHREAD_MUTEX_SETKIND
#  undef  Q_NORMAL_MUTEX_TYPE
#  undef  Q_RECURSIVE_MUTEX_TYPE
#elif defined(_OS_IRIX_)
#  define Q_HAS_RECURSIVE_MUTEX
#  undef  Q_USE_PTHREAD_MUTEX_SETKIND
#  define Q_NORMAL_MUTEX_TYPE PTHREAD_MUTEX_ERRORCHECK
#  define Q_RECURSIVE_MUTEX_TYPE PTHREAD_MUTEX_RECURSIVE
#else
// Fall through for systems we don't know about
// #  warning "Assuming non-POSIX 1003.1c thread implementation. Talk to qt-bugs@trolltech.com."
#  undef  Q_HAS_RECURSIVE_MUTEX
#  undef  Q_USE_PTHREAD_MUTEX_SETKIND
#  undef  Q_NORMAL_MUTEX_TYPE
#  undef  Q_RECURSIVE_MUTEX_TYPE
#endif


static QMutex *dictMutex = 0;
#ifdef QWS
static QPtrDict<QThread> *thrDict = 0;
#else
static QIntDict<QThread> *thrDict = 0;
#endif


extern "C" { }


#if defined(_OS_SOLARIS_)


/* moc-stub */
// Function usleep() is in C library but not in header files on Solaris 2.5.1.
// Not really a surprise, usleep() is specified by XPG4v2 and XPG4v2 is only
// supported by Solaris 2.6 and better.
// So we are trying to detect Solaris 2.5.1 using macro _XOPEN_UNIX which is
// defined by <unistd.h> when XPG4v2 is supported.
#if !defined(_XOPEN_UNIX)
typedef unsigned int useconds_t;
extern "C" int usleep(useconds_t);
#endif


class QMutexPrivate {
public:
    mutex_t mutex;

    QMutexPrivate(bool recursive = FALSE)
    { }

    virtual ~QMutexPrivate()
    { }

    virtual void lock()
    { }

    virtual void unlock()
    { }

    virtual bool locked()
    { }

#if defined(CHECK_RANGE) || !defined(Q_HAS_RECURSIVE_MUTEX)
    virtual int type() const { }
#endif
};


class QRMutexPrivate : public QMutexPrivate
{
public:
    int count;
    HANDLE owner;
    mutex_t mutex2;

    QRMutexPrivate()
	: QMutexPrivate(TRUE)
    { }

    ~QRMutexPrivate()
    { }

    void lock()
    { }

    void unlock()
    { }

    bool locked()
    { }

#if defined(CHECK_RANGE) || !defined(Q_HAS_RECURSIVE_MUTEX)
    int type() const { }
#endif
};


class QThreadPrivate {
public:
    thread_t thread_id;
    QWaitCondition thread_done;
    bool finished, running;

    QThreadPrivate()
	: thread_id(0), finished(FALSE), running(FALSE)
    { }

    ~QThreadPrivate()
    { }

    void init(QThread *that)
    { }

    static void internalRun(QThread *that)
    { }
};


class QWaitConditionPrivate {
public:
    cond_t cond;
    QMutex mutex;

    QWaitConditionPrivate()
    { }

    ~QWaitConditionPrivate()
    { }

    void wakeOne()
    { }

    void wakeAll()
    { }

    bool wait(unsigned long time)
    { }

    bool wait(QMutex *mtx, unsigned long time)
    { }
};


#else // ! defined(_OS_SOLARIS_)


/* moc-stub */
#if defined(_OS_OSF_)
// Not available in the <unistd.h> header file of Tru64 4.0F.
// Fixed in the <unistd.h> header of Tru64 5.0A so we copy/paste from there...
#if defined(_XOPEN_SOURCE) && defined(_OSF_SOURCE)
extern "C" int usleep(useconds_t);
#endif
#endif


class QMutexPrivate {
public:
#if defined (_OS_SOLARIS_)
    mutex_t mutex;
#else
    pthread_mutex_t mutex;
#endif

    QMutexPrivate(bool recursive = FALSE)
    { }

    virtual ~QMutexPrivate()
    { }

    virtual void lock()
    { }

    virtual void unlock()
    { }

    virtual bool locked()
    { }

#if defined(CHECK_RANGE) || !defined(Q_HAS_RECURSIVE_MUTEX)
    virtual int type() const { }
#endif
};


class QRMutexPrivate : public QMutexPrivate
{
public:
#ifndef Q_HAS_RECURSIVE_MUTEX
    int count;
    HANDLE owner;
    pthread_mutex_t mutex2;

    ~QRMutexPrivate()
    { }

    void lock()
    { }

    void unlock()
    { }

    bool locked()
    { }
#endif

    QRMutexPrivate()
	: QMutexPrivate(TRUE)
    { }

#if defined(CHECK_RANGE) || !defined(Q_HAS_RECURSIVE_MUTEX)
    int type() const { }
#endif
};


class QThreadPrivate {
public:
    pthread_t thread_id;
    QWaitCondition thread_done;      // Used for QThread::wait()
    bool finished, running;

    QThreadPrivate()
	: thread_id(0), finished(FALSE), running(FALSE)
    { }

    ~QThreadPrivate()
    { }

    void init(QThread *that)
    { }

    static void internalRun(QThread *that)
    { }
};


class QWaitConditionPrivate {
public:
    pthread_cond_t cond;
    QMutex mutex;

    QWaitConditionPrivate()
    { }

    ~QWaitConditionPrivate()
    { }

    void wakeOne()
    { }

    void wakeAll()
    { }

    bool wait(unsigned long time)
    { }

    bool wait(QMutex *mtx, unsigned long time)
    { }
};


#endif // defined(_OS_SOLARIS_)


extern "C" { }


#endif // QTHREAD_P_H
