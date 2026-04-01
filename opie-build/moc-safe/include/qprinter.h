/**********************************************************************
** $Id: qt/src/kernel/qprinter.h   2.3.10   edited 2005-01-24 $
**
** Definition of QPrinter class
**
** Created : 940927
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

#ifndef QPRINTER_H
#define QPRINTER_H

#ifndef QT_H
#include "qpaintdevice.h"
#include "qstring.h"
#endif // QT_H

#ifndef QT_NO_PRINTER

#if defined(B0)
#undef B0 // Terminal hang-up.  We assume that you do not want that.
#endif

class Q_EXPORT QPrinter : public QPaintDevice
{
public:
    QPrinter();
   ~QPrinter();

    enum Orientation { };

    enum PageSize    { };

    enum PageOrder   { };

    enum ColorMode   { };

    QString printerName() const;
    virtual void setPrinterName( const QString &);
    bool outputToFile()	const;
    virtual void setOutputToFile( bool );
    QString outputFileName()const;
    virtual void setOutputFileName( const QString &);

    QString printProgram() const;
    virtual void setPrintProgram( const QString &);

    QString printerSelectionOption() const;
    virtual void setPrinterSelectionOption( const QString & );

    QString docName() const;
    virtual void setDocName( const QString &);
    QString creator() const;
    virtual void setCreator( const QString &);

    Orientation orientation()	const;
    virtual void setOrientation( Orientation );
    PageSize	pageSize()	const;
    virtual void setPageSize( PageSize );

    virtual void setPageOrder( PageOrder );
    PageOrder	pageOrder() const;

    virtual void setColorMode( ColorMode );
    ColorMode	colorMode() const;

    virtual void	setFullPage( bool );
    bool		fullPage() const;
    QSize	margins()	const;

    int		fromPage()	const;
    int		toPage()	const;
    virtual void setFromTo( int fromPage, int toPage );
    int		minPage()	const;
    int		maxPage()	const;
    virtual void setMinMax( int minPage, int maxPage );
    int		numCopies()	const;
    virtual void setNumCopies( int );

    bool	newPage();
    bool	abort();
    bool	aborted()	const;

    bool	setup( QWidget *parent = 0 );

protected:
    bool	cmd( int, QPainter *, QPDevCmdParam * );
    int		metric( int ) const;

#if defined(_WS_WIN_)
    virtual void	setActive();
    virtual void	setIdle();
#endif

private:
#if defined(_WS_X11_) || defined(_WS_QWS_)
    QPaintDevice *pdrv;
    int		pid;
#endif
    int		state;
    QString	printer_name;
    QString	option_string;
    QString	output_filename;
    bool	output_file;
    QString	print_prog;
    QString	doc_name;
    QString	creator_name;
    Orientation orient;
    PageSize	page_size;
    bool	to_edge;
    short	from_pg, to_pg;
    short	min_pg,	 max_pg;
    short	ncopies;
#if defined(_WS_WIN_)
    bool	viewOffsetDone;
    QPainter*   painter;
    void	readPdlg( void* );
    void	readPdlgA( void* );
#endif

private:	// Disabled copy constructor and operator=
#if defined(Q_DISABLE_COPY)
    QPrinter( const QPrinter & );
    QPrinter &operator=( const QPrinter & );
#endif
};


inline QString QPrinter::printerName() const
{ }

inline bool QPrinter::outputToFile() const
{ }

inline QString QPrinter::outputFileName() const
{ }

inline QString QPrinter::printProgram() const
{ }

inline QString QPrinter::docName() const
{ }

inline QString QPrinter::creator() const
{ }

inline QPrinter::PageSize QPrinter::pageSize() const
{ }

inline QPrinter::Orientation QPrinter::orientation() const
{ }

inline int QPrinter::fromPage() const
{ }

inline int QPrinter::toPage() const
{ }

inline int QPrinter::minPage() const
{ }

inline int QPrinter::maxPage() const
{ }

inline int QPrinter::numCopies() const
{ }

#endif // QT_NO_PRINTER

#endif // QPRINTER_H
