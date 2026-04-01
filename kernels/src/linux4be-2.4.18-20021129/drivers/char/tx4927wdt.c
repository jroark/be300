/*
 * drivers/char/tx4927wdt.c
 *
 * Watchdog driver for integrated watchdog in the TX4927 processors.
 *
 * Copyright (C) 2002 Paul Mundt <lethal@chaoticdreams.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/watchdog.h>
#include <linux/miscdevice.h>
#include <linux/reboot.h>
#include <linux/notifier.h>
#include <linux/ioport.h>
#include <linux/fs.h>

#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/tx4927.h>

#ifndef CONFIG_CPU_TX49XX
  #error "Can't use TX49XX watchdog on non-TX49XX processor."
#endif

#ifndef minor
  #define minor(x)	MINOR(x)
#endif

#define BIT(x)	(1UL << (x))

/*
 * The TX4927 consists of 3 timer channels:
 *
 * 0: Interval Timer mode (default)
 * 1: Pulse Generator mode
 * 2: Watchdog Timer mode
 *
 * The watchdog exists as the third timer channel. When in
 * watchdog mode, the first 2 timer channels are reserved.
 */
/* Registers */
#define TX4927_TMTCR2	0xf200	/* Timer Control Register 2 */
#define TX4927_TMTISR2	0xf204	/* Timer Interrupt Status Register 2 */
#define TX4927_TMCPRA2	0xf208	/* Compare Register A 2 */
#define TX4927_TMCPRB2	0xf20c	/* Reserved */
#define TX4927_TMITMR2	0xf210	/* Interval Timer Mode Register 2 */
#define TX4927_TMCCDR2	0xf220	/* Divide Cycle Register 2 */
#define TX4927_TMWGMR2	0xf230	/* Reserved */
#define TX4927_TMWTMR2	0xf240	/* Watchdog Timer Mode Register 2 */
#define TX4927_TMTRR2	0xf2f0	/* Timer Read Register 2 */

/* Flags */
#define TMTCR2_TCE	BIT(7)	/* Timer Counter Enable */
#define TMTCR2_TMODE_HI BIT(1)	/* Timer Mode (high bit) */
#define TMTCR2_TMODE_LO	BIT(0)	/* Timer Mode (low bit) */

#define TMWTMR2_TWIE	BIT(15)	/* Watchdog Timer Signaling Enable (NMI) */
#define	TMWTMR2_WDIS	BIT(7)	/* Watchdog Timer Disable */
#define TMWTMR2_TWC	BIT(0)	/* Watchdog Timer Clear */

/* General Watchdog Flags */
#define WDT_OPEN	0	/* Device is open */
#define WDT_NOWAYOUT	1	/* Don't stop the WDT on exit */
#define WDT_GEN_NMI	2	/* Generate a NMI on overflow */

static unsigned long next_heartbeat;
static unsigned long tx4927_flags = 0;
static struct watchdog_info tx4927_wdt_info;
static struct timer_list timer;

static int compare    = CONFIG_TX4927_WDT_CMP;
static int multiplier = CONFIG_TX4927_WDT_MULT;
static int divisor    = CONFIG_TX4927_WDT_DIV;

#ifdef CONFIG_TX4927_WDT_NMI
static int action = 1;
#else
static int action = 0;
#endif

#ifdef CONFIG_WATCHDOG_NOWAYOUT
static int nowayout = 1;
#else
static int nowayout = 0;
#endif

static void tx4927_wdt_start(void)
{
	u32 tmp;

	timer.expires = tx4927_in32(TX4927_TMCPRA2) / divisor;
	next_heartbeat = tx4927_in32(TX4927_TMCPRA2) * multiplier;
	add_timer(&timer);

	tmp = tx4927_in32(TX4927_TMTCR2);
	tmp &= ~TMTCR2_TMODE_LO;
	tmp |= TMTCR2_TMODE_HI;
	tx4927_out32(TX4927_TMTCR2, tmp);

	/*
	 * Trigger a NMI on overflow if desired.
	 */
	if (test_bit(WDT_GEN_NMI, &tx4927_flags)) {
		tmp = tx4927_in32(TX4927_TMWTMR2);
		tmp |= TMWTMR2_TWIE;
		tx4927_out32(TX4927_TMWTMR2, tmp);
	}

	/*
	 * Override the default overflow value if desired.
	 */
	if (compare)
		tx4927_out32(TX4927_TMCPRA2, compare);
}

static void tx4927_wdt_stop(void)
{
	u32 tmp;

	del_timer(&timer);

	tmp = tx4927_in32(TX4927_TMWTMR2);
	tmp |= TMWTMR2_WDIS;
	tx4927_out32(TX4927_TMWTMR2, tmp);

	tmp = tx4927_in32(TX4927_TMTCR2);
	tmp &= ~TMTCR2_TCE;
	tx4927_out32(TX4927_TMTCR2, tmp);
}

static void tx4927_wdt_ping(unsigned long data)
{
	u32 tmp;

	if (!time_before(jiffies, next_heartbeat))
		return;
	
	tmp = tx4927_in32(TX4927_TMWTMR2);
	tmp |= TMWTMR2_TWC;
	tx4927_out32(TX4927_TMWTMR2, tmp);

	timer.expires = tx4927_in32(TX4927_TMCPRA2) / divisor;
	add_timer(&timer);
}

static int tx4927_wdt_ioctl(struct inode *inode, struct file *file,
			    unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	    case WDIOC_GETSUPPORT:
	    	if (copy_to_user((struct watchdog_info *)arg,
				 &tx4927_wdt_info, sizeof(tx4927_wdt_info)))
			return -EFAULT;

		break;
	    case WDIOC_KEEPALIVE:
	    	next_heartbeat = tx4927_in32(TX4927_TMCPRA2) * multiplier;

		break;
	    default:
	    	return -ENOTTY;
	}

	return 0;
}

static int tx4927_wdt_open(struct inode *inode, struct file *file)
{
	if (minor(inode->i_rdev) != WATCHDOG_MINOR)
		return -ENODEV;
	if (test_and_set_bit(WDT_OPEN, &tx4927_flags))
		return -EBUSY;

	tx4927_wdt_start();

	return 0;
}

static int tx4927_wdt_release(struct inode *inode, struct file *file)
{
	if (minor(inode->i_rdev) != WATCHDOG_MINOR)
		return -ENODEV;
	if (!test_bit(WDT_NOWAYOUT, &tx4927_flags))
		tx4927_wdt_stop();

	clear_bit(WDT_OPEN, &tx4927_flags);

	return 0;
}

static ssize_t tx4927_wdt_write(struct file *file, const char *buf,
				size_t count, loff_t *ppos)
{
	if (ppos != &file->f_pos)
		return -ESPIPE;
	if (count) {
		next_heartbeat = tx4927_in32(TX4927_TMCPRA2) * multiplier;
		return 1;
	}

	return 0;
}

static int tx4927_wdt_notify_sys(struct notifier_block *this,
				 unsigned long code, void *unusued)
{
	if (code == SYS_DOWN || code == SYS_HALT)
		tx4927_wdt_stop();
	
	return NOTIFY_DONE;
}

static struct file_operations tx4927_wdt_fops = {
	owner:		THIS_MODULE,
	llseek:		no_llseek,
	write:		tx4927_wdt_write,
	ioctl:		tx4927_wdt_ioctl,
	open:		tx4927_wdt_open,
	release:	tx4927_wdt_release,
};

static struct watchdog_info tx4927_wdt_info = {
	options:	WDIOF_KEEPALIVEPING,
	identity:	"TX4927 WDT",
	firmware_version: 1,
};

static struct notifier_block tx4927_wdt_notifier = {
	notifier_call:	tx4927_wdt_notify_sys,
	priority:	0,
};

static struct miscdevice tx4927_wdt_miscdev = {
	minor:		WATCHDOG_MINOR,
	name:		"watchdog",
	fops:		&tx4927_wdt_fops,
};

static int __init tx4927_wdt_init(void)
{
	if (misc_register(&tx4927_wdt_miscdev))
		return -EINVAL;
	if (register_reboot_notifier(&tx4927_wdt_notifier)) {
		misc_deregister(&tx4927_wdt_miscdev);
		return -EINVAL;
	}

	if (nowayout)
		set_bit(WDT_NOWAYOUT, &tx4927_flags);
	if (action)
		set_bit(WDT_GEN_NMI, &tx4927_flags);

	init_timer(&timer);
	timer.function = tx4927_wdt_ping;
	timer.data = 0;

	return 0;
}

static void __exit tx4927_wdt_exit(void)
{
	unregister_reboot_notifier(&tx4927_wdt_notifier);
	misc_deregister(&tx4927_wdt_miscdev);
}

EXPORT_NO_SYMBOLS;

MODULE_AUTHOR("Paul Mundt <lethal@chaoticdreams.org>");
MODULE_DESCRIPTION("TX4927 watchdog driver");
MODULE_LICENSE("GPL");
MODULE_PARM(nowayout, "i");
MODULE_PARM_DESC(nowayout, "Watchdog can't be stopped once started. Defaults to CONFIG_WATCHDOG_NOWAYOUT.");
MODULE_PARM(multiplier, "i");
MODULE_PARM_DESC(multiplier, "Heartbeat multiplier (next heartbeat = overflow value * multiplier). Defaults to CONFIG_TX4927_WDT_MULT.");
MODULE_PARM(divisor, "i");
MODULE_PARM_DESC(divisor, "Timer divisor (next counter clear = overflow value / divisor). Defaults to CONFIG_TX4927_WDT_DIV.");
MODULE_PARM(action, "i");
MODULE_PARM_DESC(action, "Action on counter overflow (0 = reset, 1 = NMI). Defaults to reset.");
MODULE_PARM(compare, "i");
MODULE_PARM_DESC(compare, "Overflow value. Defaults to 0xffffffff.");

module_init(tx4927_wdt_init);
module_exit(tx4927_wdt_exit);

