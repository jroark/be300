/****************************************************************************
** $Id: qt/src/tools/qstring.h   2.3.10   edited 2005-01-24 $
**
** Definition of the QString class, and related Unicode
** functions.
**
** Created : 920609
**
** Copyright (C) 1992-2000 Trolltech AS.  All rights reserved.
**
** This file is part of the tools module of the Qt GUI Toolkit.
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

#ifndef QSTRING_H
#define QSTRING_H

#ifndef QT_H
#include "qcstring.h"
#endif // QT_H


/*****************************************************************************
  QString class
 *****************************************************************************/

class QRegExp;
class QString;
class QCharRef;

class Q_EXPORT QChar {
public:
    QChar();
    QChar( char c );
    QChar( uchar c );
    QChar( uchar c, uchar r );
    QChar( const QChar& c );
    QChar( ushort rc );
    QChar( short rc );
    QChar( uint rc );
    QChar( int rc );

    QT_STATIC_CONST QChar null;            // 0000
    QT_STATIC_CONST QChar replacement;     // FFFD
    QT_STATIC_CONST QChar byteOrderMark;     // FEFF
    QT_STATIC_CONST QChar byteOrderSwapped;     // FFFE
    QT_STATIC_CONST QChar nbsp;            // 00A0

    // Unicode information

    enum Category
    { };

    enum Direction
    { };

    enum Decomposition
    { };

    enum Joining
    { };

    // ****** WHEN ADDING FUNCTIONS, CONSIDER ADDING TO QCharRef TOO

    int digitValue() const;
    QChar lower() const;
    QChar upper() const;

    Category category() const;
    Direction direction() const;
    Joining joining() const;
    bool mirrored() const;
    QChar mirroredChar() const;
    QString decomposition() const;
    Decomposition decompositionTag() const;

    char latin1() const { }
    ushort unicode() const { }
#ifndef QT_NO_CAST_ASCII
    // like all ifdef'd code this is undocumented
    operator char() const { }
#endif

    bool isNull() const { }
    bool isPrint() const;
    bool isPunct() const;
    bool isSpace() const;
    bool isMark() const;
    bool isLetter() const;
    bool isNumber() const;
    bool isLetterOrNumber() const;
    bool isDigit() const;

    uchar& cell() { }
    uchar& row() { }
    uchar cell() const { }
    uchar row() const { }

    static bool networkOrdered() { }

    friend inline int operator==( char ch, QChar c );
    friend inline int operator==( QChar c, char ch );
    friend inline int operator==( QChar c1, QChar c2 );
    friend inline int operator!=( QChar c1, QChar c2 );
    friend inline int operator!=( char ch, QChar c );
    friend inline int operator!=( QChar c, char ch );
    friend inline int operator<=( QChar c, char ch );
    friend inline int operator<=( char ch, QChar c );
    friend inline int operator<=( QChar c1, QChar c2 );

private:
#if defined(_WS_X11_) || defined(_OS_WIN32_BYTESWAP_) || defined( _WS_QWS_ )
    // XChar2b on X11, ushort on _OS_WIN32_BYTESWAP_
    //### QWS must be defined on a platform by platform basis
    uchar rw;
    uchar cl;
#if defined(QT_QSTRING_UCS_4)
    ushort grp;
#endif
    enum { };
#else
    // ushort on _OS_WIN32_
    uchar cl;
    uchar rw;
#if defined(QT_QSTRING_UCS_4)
    ushort grp;
#endif
    enum { };
#endif
} Q_PACKED;

inline QChar::QChar()
{ }
inline QChar::QChar( char c )
{ }
inline QChar::QChar( uchar c )
{ }
inline QChar::QChar( uchar c, uchar r )
{ }
inline QChar::QChar( const QChar& c )
{ }
inline QChar::QChar( ushort rc )
{ }
inline QChar::QChar( short rc )
{ }
inline QChar::QChar( uint rc )
{ }
inline QChar::QChar( int rc )
{ }


inline int operator==( char ch, QChar c )
{ }

inline int operator==( QChar c, char ch )
{ }

inline int operator==( QChar c1, QChar c2 )
{ }

inline int operator!=( QChar c1, QChar c2 )
{ }

inline int operator!=( char ch, QChar c )
{ }

inline int operator!=( QChar c, char ch )
{ }

inline int operator<=( QChar c, char ch )
{ }

inline int operator<=( char ch, QChar c )
{ }

inline int operator<=( QChar c1, QChar c2 )
{ }

inline int operator>=( QChar c, char ch ) { }
inline int operator>=( char ch, QChar c ) { }
inline int operator>=( QChar c1, QChar c2 ) { }
inline int operator<( QChar c, char ch ) { }
inline int operator<( char ch, QChar c ) { }
inline int operator<( QChar c1, QChar c2 ) { }
inline int operator>( QChar c, char ch ) { }
inline int operator>( char ch, QChar c ) { }
inline int operator>( QChar c1, QChar c2 ) { }

// internal
struct Q_EXPORT QStringData : public QShared { };


class Q_EXPORT QString
{
public:
    QString();					// make null string
    QString( QChar );				// one-char string
    QString( const QString & );			// impl-shared copy
    QString( const QByteArray& );		// deep copy
    QString( const QChar* unicode, uint length ); // deep copy
#ifndef QT_NO_CAST_ASCII
    QString( const char *str );			// deep copy
#endif
    ~QString();

    QString    &operator=( const QString & );	// impl-shared copy
#ifndef QT_NO_CAST_ASCII
    QString    &operator=( const char * );	// deep copy
#endif
    QString    &operator=( const QCString& );	// deep copy
    QString    &operator=( QChar c );
    QString    &operator=( char c );

    QT_STATIC_CONST QString null;

    bool	isNull()	const;
    bool	isEmpty()	const;
    uint	length()	const;
    void	truncate( uint pos );

    void	fill( QChar c, int len = -1 );

    QString	copy()	const;

    QString arg(long a, int fieldwidth=0, int base=10) const;
    QString arg(ulong a, int fieldwidth=0, int base=10) const;
    QString arg(int a, int fieldwidth=0, int base=10) const;
    QString arg(uint a, int fieldwidth=0, int base=10) const;
    QString arg(short a, int fieldwidth=0, int base=10) const;
    QString arg(ushort a, int fieldwidth=0, int base=10) const;
    QString arg(char a, int fieldwidth=0) const;
    QString arg(QChar a, int fieldwidth=0) const;
    QString arg(const QString& a, int fieldwidth=0) const;
    QString arg(double a, int fieldwidth=0, char fmt='g', int prec=-1) const;

    QString    &sprintf( const char* format, ... )
#if defined(_CC_GNU_) && !defined(__INSURE__)
	__attribute__ ((format (printf, 2, 3)))
#endif
	;

    int		find( QChar c, int index=0, bool cs=TRUE ) const;
    int		find( char c, int index=0, bool cs=TRUE ) const;
    int		find( const QString &str, int index=0, bool cs=TRUE ) const;
    int		find( const QRegExp &, int index=0 ) const;
#ifndef QT_NO_CAST_ASCII
    int		find( const char* str, int index=0 ) const;
#endif
    int		findRev( QChar c, int index=-1, bool cs=TRUE) const;
    int		findRev( char c, int index=-1, bool cs=TRUE) const;
    int		findRev( const QString &str, int index=-1, bool cs=TRUE) const;
    int		findRev( const QRegExp &, int index=-1 ) const;
#ifndef QT_NO_CAST_ASCII
    int		findRev( const char* str, int index=-1 ) const;
#endif
    int		contains( QChar c, bool cs=TRUE ) const;
    int		contains( char c, bool cs=TRUE ) const
		    { }
#ifndef QT_NO_CAST_ASCII
    int		contains( const char* str, bool cs=TRUE ) const;
#endif
    int		contains( const QString &str, bool cs=TRUE ) const;
    int		contains( const QRegExp & ) const;

    QString	left( uint len )  const;
    QString	right( uint len ) const;
    QString	mid( uint index, uint len=0xffffffff) const;

    QString	leftJustify( uint width, QChar fill=' ', bool trunc=FALSE)const;
    QString	rightJustify( uint width, QChar fill=' ',bool trunc=FALSE)const;

    QString	lower() const;
    QString	upper() const;

    QString	stripWhiteSpace()	const;
    QString	simplifyWhiteSpace()	const;

    QString    &insert( uint index, const QString & );
    QString    &insert( uint index, const QChar*, uint len );
    QString    &insert( uint index, QChar );
    QString    &insert( uint index, char c ) { }
    QString    &append( char );
    QString    &append( QChar );
    QString    &append( const QString & );
    QString    &prepend( char );
    QString    &prepend( QChar );
    QString    &prepend( const QString & );
    QString    &remove( uint index, uint len );
    QString    &replace( uint index, uint len, const QString & );
    QString    &replace( uint index, uint len, const QChar*, uint clen );
    QString    &replace( const QRegExp &, const QString & );

    short	toShort( bool *ok=0, int base=10 )	const;
    ushort	toUShort( bool *ok=0, int base=10 )	const;
    int		toInt( bool *ok=0, int base=10 )	const;
    uint	toUInt( bool *ok=0, int base=10 )	const;
    long	toLong( bool *ok=0, int base=10 )	const;
    ulong	toULong( bool *ok=0, int base=10 )	const;
    float	toFloat( bool *ok=0 )	const;
    double	toDouble( bool *ok=0 )	const;

    QString    &setNum( short, int base=10 );
    QString    &setNum( ushort, int base=10 );
    QString    &setNum( int, int base=10 );
    QString    &setNum( uint, int base=10 );
    QString    &setNum( long, int base=10 );
    QString    &setNum( ulong, int base=10 );
    QString    &setNum( float, char f='g', int prec=6 );
    QString    &setNum( double, char f='g', int prec=6 );

    static QString number( long, int base=10 );
    static QString number( ulong, int base=10);
    static QString number( int, int base=10 );
    static QString number( uint, int base=10);
    static QString number( double, char f='g', int prec=6 );

    void	setExpand( uint index, QChar c );

    QString    &operator+=( const QString &str );
    QString    &operator+=( QChar c );
    QString    &operator+=( char c );

    // Your compiler is smart enough to use the const one if it can.
    QChar at( uint i ) const
	{ }
    QChar operator[]( int i ) const { }
    QCharRef at( uint i );
    QCharRef operator[]( int i );

    QChar constref(uint i) const
	{ }
    QChar& ref(uint i)
	{ }

    const QChar* unicode() const { }
    const char* ascii() const;
    const char* latin1() const;
    static QString fromLatin1(const char*, int len=-1);
#ifndef QT_NO_TEXTCODEC
    QCString utf8() const;
    static QString fromUtf8(const char*, int len=-1);
#endif
    QCString local8Bit() const;
    static QString fromLocal8Bit(const char*, int len=-1);
    bool operator!() const;
#ifndef QT_NO_ASCII_CAST
    operator const char *() const { }
#endif

    QString &setUnicode( const QChar* unicode, uint len );
    QString &setUnicodeCodes( const ushort* unicode_as_ushorts, uint len );
    QString &setLatin1( const char*, int len=-1 );

    int compare( const QString& s ) const;
    static int compare( const QString& s1, const QString& s2 )
	{ }

#ifndef QT_NO_DATASTREAM
    friend Q_EXPORT QDataStream &operator>>( QDataStream &, QString & );
#endif
    // new functions for BiDi
    void compose();
    QChar::Direction basicDirection();
    QString visual(int index = 0, int len = -1);

#ifndef QT_NO_COMPAT
    const char* data() const { }
#endif

    bool startsWith( const QString& ) const;

private:
    QString( int size, bool /*dummy*/ );	// allocate size incl. \0

    void deref();
    void real_detach();
    void setLength( uint pos );
    void subat( uint );
    bool findArg(int& pos, int& len) const;

    static QChar* asciiToUnicode( const char*, uint * len, uint maxlen=(uint)-1 );
    static QChar* asciiToUnicode( const QByteArray&, uint * len );
    static char* unicodeToAscii( const QChar*, uint len );

    QStringData *d;
    static QStringData* shared_null;
    static QStringData* makeSharedNull();

    friend class QConstString;
    QString(QStringData* dd, bool /*dummy*/) : d(dd) { }
};

class Q_EXPORT QCharRef {
    friend class QString;
    QString& s;
    uint p;
    QCharRef(QString* str, uint pos) : s(*str), p(pos) { }

public:
    // Most QChar operations repeated here...

    // all this is not documented: We just say "like QChar" and let it be.
#if 1
    ushort unicode() const { }
    char latin1() const { }

    // An operator= for each QChar cast constructor...
    QCharRef operator=(char c ) { }
    QCharRef operator=(uchar c ) { }
    QCharRef operator=(QChar c ) { }
    QCharRef operator=(const QCharRef& c ) { }
    QCharRef operator=(ushort rc ) { }
    QCharRef operator=(short rc ) { }
    QCharRef operator=(uint rc ) { }
    QCharRef operator=(int rc ) { }

    operator QChar () const { }

    // each function...
    bool isNull() const { }
    bool isPrint() const { }
    bool isPunct() const { }
    bool isSpace() const { }
    bool isMark() const { }
    bool isLetter() const { }
    bool isNumber() const { }
    bool isLetterOrNumber() { }
    bool isDigit() const { }

    int digitValue() const { }
    QChar lower() { }
    QChar upper() { }

    QChar::Category category() const { }
    QChar::Direction direction() const { }
    QChar::Joining joining() const { }
    bool mirrored() const { }
    QChar mirroredChar() const { }
    QString decomposition() const { }
    QChar::Decomposition decompositionTag() const { }

    // Not the non-const ones of these.
    uchar cell() const { }
    uchar row() const { }
#endif
};

inline QCharRef QString::at( uint i ) { }
inline QCharRef QString::operator[]( int i ) { }


class Q_EXPORT QConstString : private QString {
public:
    QConstString( QChar* unicode, uint length );
    ~QConstString();
    const QString& string() const { }
};


/*****************************************************************************
  QString stream functions
 *****************************************************************************/
#ifndef QT_NO_DATASTREAM
Q_EXPORT QDataStream &operator<<( QDataStream &, const QString & );
Q_EXPORT QDataStream &operator>>( QDataStream &, QString & );
#endif

/*****************************************************************************
  QString inline functions
 *****************************************************************************/

// These two move code into makeSharedNull() and deletesData()
// to improve cache-coherence (and reduce code bloat), while
// keeping the common cases fast.
//
// No safe way to pre-init shared_null on ALL compilers/linkers.
inline QString::QString() :
    d(shared_null ? shared_null : makeSharedNull())
{ }
//
inline QString::~QString()
{ }

inline QString &QString::operator=( QChar c )
{ }

inline QString &QString::operator=( char c )
{ }

inline bool QString::isNull() const
{ }

inline bool QString::operator!() const
{ }

inline uint QString::length() const
{ }

inline bool QString::isEmpty() const
{ }

inline QString QString::copy() const
{ }

inline QString &QString::prepend( const QString & s )
{ }

inline QString &QString::prepend( QChar c )
{ }

inline QString &QString::prepend( char c )
{ }

inline QString &QString::append( const QString & s )
{ }

inline QString &QString::append( QChar c )
{ }

inline QString &QString::append( char c )
{ }

inline QString &QString::setNum( short n, int base )
{ }

inline QString &QString::setNum( ushort n, int base )
{ }

inline QString &QString::setNum( int n, int base )
{ }

inline QString &QString::setNum( uint n, int base )
{ }

inline QString &QString::setNum( float n, char f, int prec )
{ }

inline QString QString::arg(int a, int fieldwidth, int base) const
{ }

inline QString QString::arg(uint a, int fieldwidth, int base) const
{ }

inline QString QString::arg(short a, int fieldwidth, int base) const
{ }

inline QString QString::arg(ushort a, int fieldwidth, int base) const
{ }

inline int QString::find( char c, int index, bool cs ) const
{ }

inline int QString::findRev( char c, int index, bool cs) const
{ }


#ifndef QT_NO_CAST_ASCII
inline int QString::find( const char* str, int index ) const
{ }

inline int QString::findRev( const char* str, int index ) const
{ }
#endif


/*****************************************************************************
  QString non-member operators
 *****************************************************************************/

Q_EXPORT bool operator!=( const QString &s1, const QString &s2 );
Q_EXPORT bool operator<( const QString &s1, const QString &s2 );
Q_EXPORT bool operator<=( const QString &s1, const QString &s2 );
Q_EXPORT bool operator==( const QString &s1, const QString &s2 );
Q_EXPORT bool operator>( const QString &s1, const QString &s2 );
Q_EXPORT bool operator>=( const QString &s1, const QString &s2 );
#ifndef QT_NO_CAST_ASCII
Q_EXPORT bool operator!=( const QString &s1, const char *s2 );
Q_EXPORT bool operator<( const QString &s1, const char *s2 );
Q_EXPORT bool operator<=( const QString &s1, const char *s2 );
Q_EXPORT bool operator==( const QString &s1, const char *s2 );
Q_EXPORT bool operator>( const QString &s1, const char *s2 );
Q_EXPORT bool operator>=( const QString &s1, const char *s2 );
Q_EXPORT bool operator!=( const char *s1, const QString &s2 );
Q_EXPORT bool operator<( const char *s1, const QString &s2 );
Q_EXPORT bool operator<=( const char *s1, const QString &s2 );
Q_EXPORT bool operator==( const char *s1, const QString &s2 );
//Q_EXPORT bool operator>( const char *s1, const QString &s2 ); // MSVC++
Q_EXPORT bool operator>=( const char *s1, const QString &s2 );
#endif

Q_EXPORT inline QString operator+( const QString &s1, const QString &s2 )
{ }

#ifndef QT_NO_CAST_ASCII
Q_EXPORT inline QString operator+( const QString &s1, const char *s2 )
{ }

Q_EXPORT inline QString operator+( const char *s1, const QString &s2 )
{ }
#endif

Q_EXPORT inline QString operator+( const QString &s1, QChar c2 )
{ }

Q_EXPORT inline QString operator+( const QString &s1, char c2 )
{ }

Q_EXPORT inline QString operator+( QChar c1, const QString &s2 )
{ }

Q_EXPORT inline QString operator+( char c1, const QString &s2 )
{ }

#if defined(_OS_WIN32_)
extern Q_EXPORT QString qt_winQString(void*);
extern Q_EXPORT const void* qt_winTchar(const QString& str, bool addnul);
extern Q_EXPORT void* qt_winTchar_new(const QString& str);
extern Q_EXPORT QCString qt_winQString2MB( const QString& s, int len=-1 );
extern Q_EXPORT QString qt_winMB2QString( const char* mb, int len=-1 );
#endif

#endif // QSTRING_H
