/* 
 * VR41xx button input driver
 *
 * Note that the way this driver does atomic access to buffers is not
 * SMP-safe.  VR41xx CPUs don't support SMP anyway.
 *
 * Copyright (c) 2000 Michael Klar <wyldfier@iname.com>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
/*
 * Changes:
 * Francois Leblanc <francois.leblanc@cev-sa.com> Wed, 27 Mar 2002
 *  - Add print version to follow updates.
 * 
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/poll.h>
#include <linux/miscdevice.h>
#include <linux/init.h>
#ifdef CONFIG_BUTTONS_DIRECT_POWEROFF
#include <linux/reboot.h>
#endif
#ifdef CONFIG_PM_SUSPEND_WAKEUP
#include <asm/power.h>
#endif
#include <linux/buttons.h>

#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/irq.h>
#include <asm/vr41xx.h>

#define VRBUTTONS_VERSION "0.1.1"

extern unsigned short gpio_btn_map[];
extern void open_gpio_buttons(void);
extern void close_gpio_buttons(void);
extern unsigned short e105_btn_map[];
extern void open_e105_buttons(void);
extern void close_e105_buttons(void);

// BUFFSIZE must be power of 2
#define BUFFSIZE 16

struct buttons_instance {
	struct buttons_instance	*next;
	struct file 		*owner;
	struct fasync_struct 	*fasyncptr;
	wait_queue_head_t	wait;
	unsigned char		head;
	unsigned char		tail;
	unsigned char		buffer[BUFFSIZE];
};

// Implicit NULL initializers:
static struct buttons_instance *buttons_head;


void add_button_data(unsigned char data)
{
	struct buttons_instance *btnptr = buttons_head;

	while (btnptr) {
// MFK *** maybe implement a mask, so not all buttons have to go to all openers
//  otherwise, might as well make the wake_up and kill_fasync global instead of per-open
		btnptr->buffer[btnptr->head++] = data;
		btnptr->head &= BUFFSIZE - 1;
		if (btnptr->head == btnptr->tail) {
			btnptr->tail++;
			btnptr->tail &= BUFFSIZE - 1;
			btnptr->buffer[btnptr->tail] |= 0x40;
		}

		wake_up_interruptible(&btnptr->wait);
		if (btnptr->fasyncptr)
			kill_fasync(&btnptr->fasyncptr, SIGIO, POLL_IN);

		btnptr = btnptr->next;
	}
	// printk("button 0x%02x\n", data);
}

static void power_btn_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	add_button_data(0xbf);
	*VR41XX_PMUINTREG = 1;
#ifdef CONFIG_BUTTONS_DIRECT_POWEROFF
	if(!buttons_head)
		machine_halt();
#endif
}

static loff_t llseek_buttons(struct file * file, loff_t offset, int origin)
{
	return -ESPIPE;
}

/*
 * Read data format (subject to change):
 * unsigned short: bit 15: press/release 1 = pressed, 0 = released
 *                 bit 14: data lost flag 1 = data lost (but this data still valid)
 *                 bit 13: reserved
 *                 bit 12: reserved
 *                 bits 0-11: button code
 */

static ssize_t read_buttons(struct file * file,
	char * buffer, size_t count, loff_t *ppos)
{
	struct buttons_instance *btnptr = file->private_data;
	unsigned char bufdat;
	unsigned short data;
	size_t retcnt = 0;

	if (count < sizeof(data))
		return -EINVAL;

	cli();

	while (btnptr->head == btnptr->tail) {
		if (file->f_flags & O_NONBLOCK) {
			sti();
			return -EAGAIN;
		}
		interruptible_sleep_on(&btnptr->wait);
		// sleep_on will sti for us
		if (signal_pending(current))
			return -ERESTARTSYS;
		cli();
	}
	do {
		bufdat = btnptr->buffer[btnptr->tail++];
		btnptr->tail &= BUFFSIZE - 1;

		sti();

		data = (bufdat & 0xc0) << 8;
#ifdef CONFIG_VR41XX_GPIO_BUTTONS
		if ((bufdat & 0x3f) < 0x20)
			data |= gpio_btn_map[bufdat & 0x1f];
		else
#endif
#ifdef CONFIG_VR41XX_E105_BUTTONS
		if ((bufdat & 0x3f) < 0x28)
			data |= e105_btn_map[bufdat & 0x07];
		else
#endif
		if ((bufdat & 0x3f) == 0x3f)
			data |= BTN_POWER;

		if(put_user(data,  (short*)(buffer + retcnt)))
			return -EFAULT;

		retcnt += sizeof(data);
		if (retcnt > count - sizeof(data))
			return retcnt;

		cli();
	} while (btnptr->head != btnptr->tail);

	sti();

	return retcnt;
}

static unsigned int poll_buttons(struct file *file, poll_table * wait)
{
	struct buttons_instance *btnptr = file->private_data;
	int comp;

	poll_wait(file, &btnptr->wait, wait);
	cli();
	comp = (btnptr->head != btnptr->tail);
	sti();
	if (comp)
		return POLLIN | POLLRDNORM;
	return 0;
}

static int ioctl_buttons(struct inode * inode, struct file * file,
	unsigned int cmd, unsigned long arg)
{
// Lots of good stuff to be added here
	return -EINVAL;
}

static int open_buttons(struct inode * inode, struct file * file)
{
	struct buttons_instance *prev = NULL, *btnptr = buttons_head;

	while (btnptr) {
		// sanity check: make sure same open hasn't already happened
		if (btnptr->owner == file)
			return -EINVAL;
		prev = btnptr;
		btnptr = btnptr->next;
	}

	btnptr = kmalloc(sizeof(struct buttons_instance), GFP_KERNEL);
	if (!btnptr)
		return -ENOMEM;

	memset(btnptr, 0, sizeof(struct buttons_instance));
	btnptr->owner = file;
	init_waitqueue_head(&btnptr->wait);

		// on first open, request IRQs and set to level trigger, active high
		// enabling IRQs before buttons_head is assigned causes any pending
		// ints to not fill buffer with data, which is probably good
	if (!prev) {
#ifdef CONFIG_VR41XX_GPIO_BUTTONS
		open_gpio_buttons();
#endif
#ifdef CONFIG_VR41XX_E105_BUTTONS
		open_e105_buttons();
#endif
		buttons_head = btnptr;
	} else {
		prev->next = btnptr;
	}
	file->private_data = btnptr;

	return 0;
}

static int fasync_buttons(int fd, struct file * file, int on)
{
	struct buttons_instance *btnptr = file->private_data;
	int retval;

	retval = fasync_helper(fd, file, on, &btnptr->fasyncptr);
	if (retval < 0)
		return retval;
	return 0;
}

static int release_buttons(struct inode * inode, struct file * file)
{
	struct buttons_instance *prev = NULL, *btnptr = buttons_head;

	fasync_buttons(-1, file, 0);

	while (btnptr) {
		if ((btnptr->owner == file) || (btnptr == file->private_data))
			break;
		prev = btnptr;
		btnptr = btnptr->next;
	}

	if (btnptr) {
		if (!prev)
			buttons_head = btnptr->next;
		else
			prev->next = btnptr->next;
		kfree(btnptr);
	} else {			// shouldn't ever happen
		if (file->private_data)
			kfree(file->private_data);
	}

		// free IRQs on last close
	if (!buttons_head) {
#ifdef CONFIG_VR41XX_GPIO_BUTTONS
		close_gpio_buttons();
#endif
#ifdef CONFIG_VR41XX_E105_BUTTONS
		close_e105_buttons();
#endif
	}

	file->private_data = NULL;

	return 0;
}

struct file_operations vr41xx_buttons_fops = {
	llseek:		llseek_buttons,
	read:		read_buttons,
	poll:		poll_buttons,
	ioctl:		ioctl_buttons,
	open:		open_buttons,
	release:	release_buttons,
	fasync:		fasync_buttons,
};

static struct miscdevice vr41xx_buttons = {
	VR41XX_BUTTONS_MINOR, "vrbuttons", &vr41xx_buttons_fops
};

int __init vr41xx_buttons_init(void)
{
	int retval;

	printk (KERN_INFO"VR41xx button input driver version "VRBUTTONS_VERSION"\n");
#ifdef CONFIG_VR41XX_E105_BUTTONS
	printk (KERN_INFO "E105 buttons mapping configured\n");
#endif
	
	retval = misc_register(&vr41xx_buttons);
	if (retval < 0)
		return retval;

	// always have power buton active, other buttons active only on first open
	if (request_irq(VR41XX_IRQ_POWER, power_btn_interrupt, SA_SAMPLE_RANDOM |
	    SA_INTERRUPT, "vrbuttons", NULL))
		printk(KERN_ERR "vrbuttons: Unable to get power button IRQ.\n");

	return 0;
}

