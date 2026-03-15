/*
 * drivers/char/serial_be300.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Serial driver for Casio Cassiopeia BE-300
 * John Roark <jroark@linux4.be>
 */

#include <linux/init.h>
#include <linux/config.h>
#include <linux/tty.h>
#include <linux/major.h>
#include <linux/console.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/serial.h>
#include <linux/serial_reg.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/delay.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/mm.h>

#include "serial_be300.h"

#define	SIU_CLOCK	0x0102
#define	DSIU_CLOCK	0x0802

extern vr41xx_clock_supply(u16);

static struct tty_driver be300_driver, be300_callout;
static int be300_refcount, be300_initialized;

static struct tty_struct **be300_tty;
static struct termios **be300_termios;
static struct termios **be300_termios_locked;

static 	int baud_table[] = {
		110, 10473,
		300,  3840,
		600,  1920,
		1200,   960,
		2400,   480,
		4800,   240,
		9600,   120,
		19200,    60,
		38400,    30,
		57600,    20,
		115200,    10,
	};

static inline unsigned int serial_inb(int offset)
{
/*	return readb((unsigned long)BE300_UART_BASE + 
		(offset * BE300_REG_SHIFT));
 */
	return readb((unsigned long)BE300_UART_BASE + 
		(offset << BE300_REG_SHIFT));
}

static inline void serial_outb(int offset, unsigned char value)
{
/*	writeb(value, (unsigned long)BE300_UART_BASE +
		(offset * BE300_REG_SHIFT));
 */
	writeb(value, (unsigned long)BE300_UART_BASE +
		(offset << BE300_REG_SHIFT));
}

static inline void serial_putc(unsigned char ch)
{
	while(!(serial_inb(UART_LSR) & UART_LSR_THRE));
	serial_outb(UART_TX, ch);
}

static inline void serial_puts(const unsigned char *buf, int count)
{
	unsigned int i = 0;

	for(i = 0; i < count; i++)
	{
		if(buf[i] == '\n')
			serial_putc('\r');
		serial_putc(buf[i]);
	}
}

#ifdef CONFIG_REMOTE_DEBUG
int putDebugChar(unsigned char byte)
{
	serial_putc(byte);
}

unsigned char getDebugChar(void)
{
	while(!(serial_inb(UART_LSR) & UART_LSR_DR));
	return serial_inb(UART_RX);
}
#endif /* CONFIG_REMOTE_DEBUG */

int be300_calc_divider(int baud)
{
	int i;

	for (i = 0; i < sizeof(baud_table) / sizeof(int); i += 2)
	{
		if (baud_table[i] == baud)
		{
			return baud_table[i + 1];
		}
	}
	return 0;
}

void be300_set_baud(int baud)
{
	int divisor = be300_calc_divider(baud);
	
	if(!divisor)
		return;
		
	serial_outb(UART_LCR, UART_LCR_DLAB);	
	serial_outb(UART_DLL, divisor & 0xFF);
	serial_outb(UART_DLM, (divisor & 0xff00) >> 8);
	serial_outb(UART_LCR, UART_LCR_DLAB);
	
	/*serial_inb(UART_IER);*/
}

static void be300_show_serial_version(void)
{
	printk("Casio Cassiopeia BE-300 Serial Driver version 0.01\n");
}

static int be300_serial_chars_in_buffer(struct tty_struct *tty)
{
//printk("* chars_in_buffer *\n");
	return 0;
}

static void be300_serial_put_char(struct tty_struct *tty, unsigned char ch)
{
//printk("* put_char *\n");
	serial_putc(ch);
}

static int be300_serial_write(struct tty_struct *tty, int from_user,
	const unsigned char *buf, int count)
{
//printk("* write *\n");
	serial_puts(buf, count);	
	return count;
}

static int be300_serial_write_room(struct tty_struct *tty)
{
//printk("* write_room *\n");
	return 1;
}

static void be300_serial_flush_buffer(struct tty_struct *tty)
{
//printk("* flush_buffer *\n");
}

static void be300_serial_flush_chars(struct tty_struct *tty)
{
//printk("* flush_chars *\n");
}

static void be300_serial_stop(struct tty_struct *tty)
{
//printk("* stop *\n");
}

static void be300_serial_start(struct tty_struct *tty)
{
//printk("* start *\n");
}

static void be300_serial_hangup(struct tty_struct *tty)
{
//printk("* hangup *\n");
}

static void be300_serial_throttle(struct tty_struct *tty)
{
//printk("* throttle *\n");
}

static void be300_serial_unthrottle(struct tty_struct *tty)
{
//	printk("unthrottle: %d...\n",
//	       tty->ldisc.chars_in_buffer(tty));
}

static void be300_serial_close(struct tty_struct *tty, struct file *flip)
{
	MOD_DEC_USE_COUNT;
//printk("* close *\n");
}

static int be300_serial_open(struct tty_struct *tty, struct file *flip)
{
	/*int retval;
	unsigned long flags;*/
	
	printk("* open *\n");
	
	MOD_INC_USE_COUNT;
	
	if(!be300_initialized)
		return -EIO;
		
	if(tty->index < 0)
		return -ENODEV;
		
	/*save_flags(flags);*/
	
	return 0;
}

static void be300_serial_set_termios(struct tty_struct *tty, 
	struct termios *old_termios)
{
//printk("* set_termios *\n");
}

static int be300_serial_ioctl(struct tty_struct * tty, struct file * filp, 
	unsigned int cmd, unsigned long arg)
{
	int /*ival,*/ rc = 0;
	
	switch(cmd)
	{
		case TIOCGSOFTCAR:
		printk("* ioctl: TIOCGSOFTCAR *\n");
			break;
			
		case TIOCSSERIAL:
		printk("* ioctl: TIOCSSERIAL *\n");
			break;
			
		case TIOCGSERIAL:	
		printk("* ioctl: TIOCGSERIAL *\n");
			break;
			
		case TIOCSSOFTCAR:
		printk("* ioctl: TIOCSSOFTCAR *\n");
			break;

		case TCSBRK:
			break;
		case TCSBRKP:
			break;
		case TIOCMGET:
			break;
		case TIOCMBIS:
		case TIOCMBIC:
		case TIOCMSET:
			break;
			
		default:
		printk("* ioctl: default *\n");
			rc = -ENOIOCTLCMD;
			break;
	}

	return -ENOIOCTLCMD;/* rc;*/
}

static inline irqreturn_t be300_uart_interrupt(int irq, void *data, 
	struct pt_regs *regs)
{
	unsigned char status;
	
	printk("be300_uart_interrupt called\n");
	
	/* check the interrupt status */
	if(!((status = serial_inb(UART_IIR)) & UART_IIR_NO_INT))
	{
		if(status & UART_LSR_DR)
		{
			/* Receiver data ready */
			printk("* Receiver data ready *\n");
		}
		if(status & UART_LSR_THRE)
		{
			/* Transmitter holding register empty */
			printk("* Transmitter holding register empty *\n");
		}
	}
	return IRQ_HANDLED;
}

void __init be300_serial_init(void)
{
	int flags;
	
	be300_show_serial_version();

	/* Allocate critical structures */
	if(!(be300_tty = kmalloc(sizeof(struct tty_struct), GFP_KERNEL)))
	{
		return;
	}
	if(!(be300_termios = kmalloc(sizeof(struct termios), GFP_KERNEL)))
	{
		kfree(be300_tty);
		return;
	}
	if(!(be300_termios_locked = kmalloc(sizeof(struct termios), GFP_KERNEL)))
	{
		kfree(be300_termios);
		kfree(be300_tty);
		return;
	}
	
	memset(be300_tty, 0, sizeof(struct tty_struct));
	memset(be300_termios, 0, sizeof(struct termios));
	memset(be300_termios_locked, 0, sizeof(struct termios));
	memset(&be300_driver, 0, sizeof(struct tty_driver));
	memset(&be300_callout, 0, sizeof(struct tty_driver));
	
	be300_driver.magic = TTY_DRIVER_MAGIC;
#ifdef CONFIG_DEVFS_FS
	be300_driver.name = "tts/%d";
#else
	be300_driver.name = "ttyS";
#endif /* CONFIG_DEVFS_FS */
	be300_driver.driver_name = "serial_be300";
	be300_driver.num = 1;
	be300_driver.type = TTY_DRIVER_TYPE_SERIAL;
	be300_driver.subtype = SERIAL_TYPE_NORMAL;
	be300_driver.major = TTY_MAJOR;
	be300_driver.minor_start = 64;
	be300_driver.init_termios = tty_std_termios;
	be300_driver.init_termios.c_cflag = B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	be300_driver.refcount = &be300_refcount;
	be300_driver.flags = TTY_DRIVER_REAL_RAW;
	
	be300_driver.ttys = be300_tty;
	be300_driver.termios = be300_termios;
	be300_driver.termios_locked = be300_termios_locked;
	
	be300_driver.open = be300_serial_open;
	be300_driver.close = be300_serial_close; 
	be300_driver.write = be300_serial_write;
	be300_driver.put_char = be300_serial_put_char;
	be300_driver.flush_chars = be300_serial_flush_chars;
	be300_driver.write_room = be300_serial_write_room;
	be300_driver.chars_in_buffer = be300_serial_chars_in_buffer;
	be300_driver.flush_buffer = be300_serial_flush_buffer;
	be300_driver.ioctl = be300_serial_ioctl;
	be300_driver.throttle = be300_serial_throttle;
	be300_driver.unthrottle = be300_serial_unthrottle;
	be300_driver.set_termios = be300_serial_set_termios;
	be300_driver.stop = be300_serial_stop;
	be300_driver.start = be300_serial_start;
	be300_driver.hangup = be300_serial_hangup;
	
	be300_callout = be300_driver;
#ifdef CONFIG_DEVFS_FS
	be300_callout.name = "cua/%d";
#else
	be300_callout.name = "cua";
#endif
	be300_callout.major = TTYAUX_MAJOR;
//	be300_callout.subtype = SERIAL_TYPE_CALLOUT;
	
	
	if(tty_register_driver(&be300_driver))
		panic("Unable to register serial driver\n");
	if(tty_register_driver(&be300_callout))
		panic("Unable to register callout driver\n");
	
/*	vr41xx_clock_supply(SIU_CLOCK);
	vr41xx_clock_supply(DSIU_CLOCK);
*/	
	save_and_cli(flags);
	
	if(request_irq(0x13, be300_uart_interrupt, SA_INTERRUPT,
		"BE300Uart", NULL))
		panic("Cannot allocate IRQ for UART.\n");
	else
		printk("ttyS0 at 0xAA008680 (irq = 19) is a NEC D89041F1001 UART\n"); 
	/* clear the interupt regs for luck */
	(void)serial_inb(UART_LSR);
	(void)serial_inb(UART_RX);
	(void)serial_inb(UART_IIR);
	(void)serial_inb(UART_MSR);

	restore_flags(flags);
	
	be300_initialized = 1;
}

#ifdef CONFIG_SERIAL_BE300_CONSOLE
static void be300_serial_console_write(struct console *co, const char *s, 
	unsigned count)
{
	serial_puts(s, count);
}

static struct tty_driver *be300_serial_console_device(struct console *co, int *index)
{
	return &be300_driver;
	//return MKDEV(TTY_MAJOR, 64 + co->index);
}

/* recieve a char from the serial port 
 */
static int be300_serial_console_wait_key(struct console *co)
{
	while(!(serial_inb(UART_LSR) & UART_LSR_DR));
	return serial_inb(UART_RX);
}

static __init int be300_serial_console_setup(struct console *co, char *options)
{
/*	uint32 baud = 9600;
	uint8 bits = 0x3;
	uint8 parity = 0;
	uint8 stop = 0;
	uint32 divisor = 1;

	serial_outb(UART_LCR, 0x0);
	serial_outb(UART_IIR, 0);
	
	serial_outb(UART_LCR, UART_LCR_DLAB);
	serial_outb(UART_DLL, divisor & 0xff);
	serial_outb(UART_DLM, (divisor & 0xff00)>>8);

	serial_outb(UART_LCR, 0x0);
	
	serial_outb(UART_LCR, bits | parity | stop);
*/
	return 0;
}

static struct console be300_sercons = {
	name:		"ttyS",
	write:		be300_serial_console_write,
	/* int *(read)(struct console *, const char *, unsigned); */
	device:		be300_serial_console_device,
	//wait_key:	be300_serial_console_wait_key,
	/* void *(unblank)(void); */
	setup:		be300_serial_console_setup,
	flags:		CON_PRINTBUFFER,
	index:		-1
	/* int cflag; */
};

void __init be300_console_init(void)
{
	register_console(&be300_sercons);
}
#endif /* CONFIG_SERIAL_BE300_CONSOLE */
