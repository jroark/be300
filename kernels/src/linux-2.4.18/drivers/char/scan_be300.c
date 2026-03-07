/*
 *	$Id: scan_be300.c,v 1.3 2002/12/12 11:32:05 jroark Exp $ 
 *
 *  Very basic keyboard driver for be300
 *  First written by Marc <themaw@linux4.be>
 *  updated by John Roark <jroark@linux4.be>
 *
 *  Adapted from: scan_keyb.c:
 *	Copyright (C) 2000 YAEGASHI Takeshi
 *	Generic scan keyboard driver
 *
 * 12082002 - marc - created
 * 12092002 - jroark - updated to scroll through all keycodes
 ************************************************************
 * Current typing scheme
 ************************************************************
 * rkt = no action (it is used to shift other btns actions)
 * ok = enter
 * esc = tab
 * pwr = ctrl + alt + delete
 * up = up
 * down = down
 * right = right
 * left = left
 *
 * rkt + ok = space
 * rkt + esc = backspace
 * rkt + pwr = ctrl + c
 * rkt + up = next alpha numeric char
 * rkt + down = previous alpha numeric char
 * rkt + right = select current alpha numeric and move to next
 * rkt + left = overwrite previous alpha numeric char
 */

#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/tty.h>
#include <linux/mm.h>
#include <linux/signal.h>
#include <linux/init.h>
#include <linux/kbd_ll.h>
#include <linux/delay.h>
#include <linux/random.h>
#include <linux/poll.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/kbd_kern.h>
#include <linux/timer.h>
#include <asm/io.h>

#define readb(addr)                (*(volatile unsigned char *)(addr))

#define SCANHZ	(HZ/5)

static int caps = 0;
struct timer_list scan_timer;

extern void ctrl_alt_del(void);

#define NUM_SCANCODES	48
const unsigned char scan_codes[NUM_SCANCODES] = 
/* a - j */
{ 0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 
/* k - t */
  0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14,
/* u - z & space*/
  0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c, 0x39,
/* 0 - 9 */
  0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
/*` & ~ - & _ = & + [ & { ] & } ; & : ' & " \ & | , & < . & > */
  0x29, 0x0c, 0x0d, 0x1a, 0x1b, 0x27, 0x28, 0x2b, 0x33, 0x34,
/*/ & ? */
  0x35 };


int kbd_leds(unsigned char leds)
{
// nothing in here ..
}

static void toggle_caps(void)
{
	if(caps)
	{
		handle_scancode(0x2a, 0);
		caps = 0;
	}
	else
	{
		handle_scancode(0x2a, 1);
		caps = 1;
	}
}

static void scan_kbd(unsigned long dummy)
{
	static int pos = 0;
	unsigned char set1 = 0, set2 = 0;

	set1 = readb(0xAA00A042);
	set2 = readb(0xAA00A043);

	if((set2 & 0x10) == 0x10)
	{
		/* rocket btn is down */
		if((set1 & 0x10) == 0x10)
		{
			/* up is down */
			handle_scancode(0x53, 1);
			handle_scancode(0x53, 0);
			handle_scancode(scan_codes[pos], 1);
			handle_scancode(scan_codes[pos], 0);
			handle_scancode(105, 1);
			handle_scancode(105, 0);
			/* increment pos */
			if(pos >= (NUM_SCANCODES - 1)) 
			{
				pos = 0;
				toggle_caps();
			}
			else pos++;
		}
		else if((set1 & 0x20) == 0x20)
		{
			/* down is down */
			handle_scancode(0x53, 1);
			handle_scancode(0x53, 0);
			handle_scancode(scan_codes[pos], 1);
			handle_scancode(scan_codes[pos], 0);
			handle_scancode(105, 1);
			handle_scancode(105, 0);
			/* decrement pos */
			if(pos <= 0) 
			{
				pos = (NUM_SCANCODES - 1);
				toggle_caps();
			}
			else pos--;
		}
		else if((set1 & 0x40) == 0x40)
		{
			/* right is down */
			handle_scancode(106, 1);
			handle_scancode(106, 0);
		}
		else if((set1 & 0x80) == 0x80)
		{
			/* left is down */
			handle_scancode(105, 1);
			handle_scancode(105, 0);
		}
		else if((set1 & 0x04) == 0x04)
		{
			/* ok is down */
			/* send space */
			handle_scancode(0x39, 1);
			handle_scancode(0x39, 0);
		}
		else if((set1 & 0x08) == 0x08)
		{
			/* esc is down */
			/* backspace */
			handle_scancode(105, 1);
			handle_scancode(105, 0);
			handle_scancode(0x53, 1);
			handle_scancode(0x53, 0);
		}
		else if((set2 & 0x80) == 0x80)
		{
			/* pwr is down */
			/* ctrl c */
			handle_scancode(0x1d, 1);
			handle_scancode(0x2e, 1);
			handle_scancode(0x2e, 0);
			handle_scancode(0x1d, 0);
		}
	}
	else
	{
		if((set1 & 0x04) == 0x04)
		{
			/* ok = enter */
			handle_scancode(28, 1);
			handle_scancode(28, 0);
		}
		if((set1 & 0x08) == 0x08)
		{
			/* esc = tab */
			handle_scancode(15, 1);
			handle_scancode(15, 0);
		}
		if((set1 & 0x10) == 0x10)
		{
			/* up */
			handle_scancode(103, 1);
			handle_scancode(103, 0);
		}
		if((set1 & 0x20) == 0x20)
		{
			/* down */
			handle_scancode(108, 1);
			handle_scancode(108, 0);
		}
		if((set1 & 0x40) == 0x40)
		{
			/* right */
			handle_scancode(106, 1);
			handle_scancode(106, 0);
		}
		if((set1 & 0x80) == 0x80)
		{
			/* left */
			handle_scancode(105, 1);
			handle_scancode(105, 0);
		}
		if((set2 & 0x80) == 0x80)
		{
			/* reset */
			ctrl_alt_del();
		}
	}

	init_timer(&scan_timer);
	scan_timer.expires = jiffies + SCANHZ;
	scan_timer.data = 0;
	scan_timer.function = scan_kbd;
	add_timer(&scan_timer);
}


int kbd_setkeycode(unsigned int scancode, unsigned int keycode)
{
        return (scancode == keycode) ? 0 : -EINVAL;
}
        
int kbd_getkeycode(unsigned int scancode)
{
	return scancode;
}
                

			      
char kbd_unexpected_up(unsigned char keycode)
{
	return 0x80;
}		

int kbd_translate(unsigned char scancode, unsigned char *keycode,
        char raw_mode)
{
        *keycode = scancode;
                
	return 1;
}
                        
			      
void __init kbd_init_hw(void)
{

	init_timer(&scan_timer);
	scan_timer.expires = jiffies + SCANHZ;
	scan_timer.data = 0;
	scan_timer.function = scan_kbd;
	add_timer(&scan_timer);

	printk(KERN_INFO "Generic scan keyboard driver initialized\n");
}
