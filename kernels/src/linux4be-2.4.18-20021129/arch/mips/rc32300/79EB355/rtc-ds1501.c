/*
 * BRIEF MODULE DESCRIPTION
 *      DS1501 (Dallas Semiconductor) Real Time Clock Support
 *
 * Copyright 2002 MontaVista Software Inc.
 * Author: MontaVista Software, Inc.
 *         	stevel@mvista.com or source@mvista.com
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 *  THIS  SOFTWARE  IS PROVIDED   ``AS  IS'' AND   ANY  EXPRESS OR IMPLIED
 *  WARRANTIES,   INCLUDING, BUT NOT  LIMITED  TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
 *  NO  EVENT  SHALL   THE AUTHOR  BE    LIABLE FOR ANY   DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 *  NOT LIMITED   TO, PROCUREMENT OF  SUBSTITUTE GOODS  OR SERVICES; LOSS OF
 *  USE, DATA,  OR PROFITS; OR  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 *  ANY THEORY OF LIABILITY, WHETHER IN  CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 *  THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the  GNU General Public License along
 *  with this program; if not, write  to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/types.h>
#include <linux/time.h>
#include <asm/time.h>
#include <asm/addrspace.h>
#include <asm/debug.h>
#include <asm/rc32300/rc32300.h>

#undef BCD_TO_BIN
#define BCD_TO_BIN(val) (((val)&15) + ((val)>>4)*10)

#undef BIN_TO_BCD
#define BIN_TO_BCD(val) ((((val)/10)<<4) + (val)%10)

#define EPOCH 2000

static unsigned long
rtc_ds1501_get_time(void)
{	
	u32 century, year, month, day, hour, minute, second;

	/* read time data */
	writeb(readb(&rtc->control_b) & TDC_DIS_BUFF, &rtc->control_b);
	century  = BCD_TO_BIN(readb(&rtc->century));
	year = BCD_TO_BIN(readb(&rtc->year)) + (century * 100);
	month = BCD_TO_BIN(readb(&rtc->month) & 0x1f);
	day = BCD_TO_BIN(readb(&rtc->date));
	hour = BCD_TO_BIN(readb(&rtc->hours) & 0x3f);	/* 24 hour format */
	minute = BCD_TO_BIN(readb(&rtc->mins));
	second = BCD_TO_BIN(readb(&rtc->secs));
	writeb(readb(&rtc->control_b) | TDC_ENA_BUFF, &rtc->control_b);

	return mktime(year, month, day, hour, minute, second);
}

static int 
rtc_ds1501_set_time(unsigned long t)
{
	struct rtc_time tm;
	u8 temp, ctrl;
	u8 year, month, day, hour, minute, second;

	/* freeze external registers */
	ctrl = readb(&rtc->control_b);
	writeb(ctrl & TDC_DIS_BUFF, &rtc->control_b);

	/* convert */
	to_tm(t, &tm);

	/* check each field one by one */

	writeb(BIN_TO_BCD(EPOCH/100), &rtc->century);
	year = BIN_TO_BCD(tm.tm_year - EPOCH);
	if (year != readb(&rtc->year))
		writeb(year, &rtc->year);

	temp = readb(&rtc->month);
	month = BIN_TO_BCD(tm.tm_mon + 1); /* tm_mon starts from 0 to 11 */
	if (month != (temp & 0x1f))
		writeb((month & 0x1f) | (temp & ~0x1f), &rtc->month);

	day = BIN_TO_BCD(tm.tm_mday);
	if (day != readb(&rtc->date))
		writeb(day, &rtc->date);

	hour = BIN_TO_BCD(tm.tm_hour) & 0x3f;	/* 24 hour format */
	if (hour != readb(&rtc->hours))
		writeb(hour, &rtc->hours);

	minute = BIN_TO_BCD(tm.tm_min);
	if (minute != readb(&rtc->mins))
		writeb(minute, &rtc->mins);

	second = BIN_TO_BCD(tm.tm_sec);
	if (second != readb(&rtc->secs))
		writeb(second, &rtc->secs);
	
	writeb(ctrl | TDC_ENA_BUFF, &rtc->control_b);
	return 0;
}

void
rtc_ds1501_init(void)
{
	u8 stop;

	writeb(readb(&rtc->control_b) & TDC_DIS_BUFF, &rtc->control_b);
	stop = readb(&rtc->month);
	writeb(readb(&rtc->control_b) | TDC_ENA_BUFF, &rtc->control_b);

	if (stop & TDS_STOP) {
		/* start clock */
		writeb(readb(&rtc->control_b) & TDC_DIS_BUFF, &rtc->control_b);
		writeb(stop | TDS_STOP, &rtc->month);
		writeb(readb(&rtc->control_b) | TDC_ENA_BUFF, &rtc->control_b);
		/* wait 2s till oscillator stabilizes */
		mdelay(2000);
	}
	
	/* set the function pointers */
	rtc_get_time = rtc_ds1501_get_time;
	rtc_set_time = rtc_ds1501_set_time;
}
