/*
 * BRIEF MODULE DESCRIPTION
 *	IDT 79EB355 lcd support.
 *
 * Copyright 2002 MontaVista Software Inc.
 * Author: MontaVista Software, Inc.
 *         	stevel@mvista.com or source@mvista.com
 *
 *  Modified slightly from version by IDT:
 *
 *  Copyright  a 2000 by Integrated Device Technology, Inc.
 *  This software is the property of the Integrated Device
 *  Technology, Inc.  (IDT).
 *  IDT MAKES NO WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 *  WITH REGARD TO THIS SOFTWARE.  IN NO EVENT SHALL IDT
 *  BE LIABLE FOR INCIDENTAL OR CONSEQUENTIAL DAMAGES IN
 *  CONNECTION WITH OR ARISING FROM THE FURNISHING, PERFORMANCE,
 *  OR USE OF THIS SOFTWARE.
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

#include <linux/init.h>
#include <linux/irq.h>
#include <linux/ioport.h>
#include <asm/bootinfo.h>
#include <asm/io.h>
#include <asm/rc32300/rc32300.h>

#define LCD_FUNC_SET    	0x38
#define	LCD_ENT_MODE    	0x06
#define	LCD_DISP_ON_CURS_OFF 	0x0C 
#define	LCD_DISP_ON_CURS_ON 	0x0E 
#define	LCD_DISP_CLEAR  	0x01
#define	LCD_DDRAM_ADR_SET_0	0x80
#define	LCD_DDRAM_ADR_SET_1	0xC0

#define	LCD_MAX_CHAR_PER_LINE 	16
#define	LCD_MAX_LINES	 	2


static void delay_lcd_long(void)
{
	int ii ;
	for (ii = 0 ; ii < 0x5FF ; ii++)
		;
}

static void delay_lcd_short(void)
{
	int ii ;
	for (ii = 0 ; ii < 0x1FF ; ii++)
		;
}


int init_lcd(void)
{
	u8* chLcdBase = (u8*)KSEG1ADDR(LCD_BASE);

	/*-------------- Reset LCD ---------------------------*/

	delay_lcd_long() ;

	/* send function select 4 times */
	writeb(LCD_FUNC_SET, chLcdBase);
	delay_lcd_long() ;
	writeb(LCD_FUNC_SET, chLcdBase);
	delay_lcd_long() ;
	writeb(LCD_FUNC_SET, chLcdBase);
	delay_lcd_long() ;
	writeb(LCD_FUNC_SET, chLcdBase);
	delay_lcd_long() ;

	writeb(LCD_ENT_MODE, chLcdBase);
	delay_lcd_long() ;

	writeb(LCD_DISP_ON_CURS_OFF, chLcdBase);
	delay_lcd_long() ;

	writeb(LCD_DISP_CLEAR, chLcdBase);
	delay_lcd_long() ;

	/* set it to row-0, column-0 */
	writeb(LCD_DDRAM_ADR_SET_0, chLcdBase);
	delay_lcd_long() ;

	return 0;
}


int idtprintf(const char *fmt, ...)
{
	va_list args;
	u8* chLcdData = (u8*)KSEG1ADDR((LCD_BASE+1));
	u8* chLcdBase = (u8*)KSEG1ADDR(LCD_BASE);
	char str[LCD_MAX_CHAR_PER_LINE * LCD_MAX_LINES];
	int iNumChars, iCharsSent;

	va_start(args, fmt);
	vsprintf(str, fmt, args);
	va_end(args);

	iNumChars = strlen(str);

	/* Can't have more characters than what will fit on the display */
	if (iNumChars > (LCD_MAX_CHAR_PER_LINE * LCD_MAX_LINES))
		iNumChars = (LCD_MAX_CHAR_PER_LINE * LCD_MAX_LINES) ;

	/* first blank out the whole display */
	writeb(LCD_DISP_CLEAR, chLcdBase);
	delay_lcd_long() ;

	/* set it to row-0, column-0 */
	writeb(LCD_DDRAM_ADR_SET_0, chLcdBase);
	delay_lcd_long() ;

	/* Now display characters one by one*/
	for(iCharsSent = 0 ; iCharsSent < iNumChars ; iCharsSent++) {
		if (iCharsSent == LCD_MAX_CHAR_PER_LINE) {
			/* move over to the next line */
			writeb(LCD_DDRAM_ADR_SET_1, chLcdBase); 
			delay_lcd_long() ;
		}
		writeb(str[iCharsSent], chLcdData);
		delay_lcd_short() ;
	}

	return iNumChars;
}
