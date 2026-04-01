/* 
 * VR41xx Touch Panel Driver
 *
 * Copyright (c) 1999,2000 Michael Klar, mfklar@ponymail.com
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */

// This is used to enable the /proc/piuinfo interface to return
// diagnostic info on PIU state and events:
// #define DEBUG_TIMING_PARAMS

#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/poll.h>
#include <linux/miscdevice.h>
#include <linux/random.h>
#include <linux/init.h>
#include <linux/pm.h>

#ifdef DEBUG_TIMING_PARAMS
#include <linux/proc_fs.h>
#endif

#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/irq.h>
#include <asm/vr41xx.h>

#include <linux/tpanel.h>

#define TPANEL_VERSION "0.1.0"

// BUFFSIZE can be increased, but it must be a power of 2

#define BUFFSIZE 128

struct tpanel_status {
	unsigned int	buffer[BUFFSIZE];
	unsigned char	head;
	unsigned char	tail;
	unsigned char	lostflag;
	unsigned char	lastcontact;
	wait_queue_head_t wait;
	struct fasync_struct *fasyncptr;
	int		active;
#ifdef DEBUG_TIMING_PARAMS
	unsigned short	orig_scanint;
	unsigned short	orig_settletime;
	unsigned int	dropped_count;
	unsigned int	overflow_count;
	unsigned int	incompletescan_count;
	unsigned int	dualpagevalid_count;
	unsigned int	unhandledint_count;
#endif
};

static struct tpanel_status tpanel;

static void tpanel_add_data(unsigned int data1, unsigned int data2)
{
	unsigned int errcnt;

	tpanel.buffer[tpanel.head++] = data1 | ((unsigned int)tpanel.lostflag << 28);
	tpanel.buffer[tpanel.head++] = data2;
	tpanel.head &= (BUFFSIZE - 1);
	if (tpanel.head == tpanel.tail) {
		errcnt = tpanel.buffer[tpanel.tail] & 0xff;
		if (!(tpanel.buffer[tpanel.tail] & 0x80000000)) {
			errcnt++;
			if (!(errcnt & 0xff))
				errcnt = 0xff;
		}
		errcnt |= tpanel.buffer[tpanel.tail] & 0x10000000;
		tpanel.tail += 2;
		tpanel.tail &= (BUFFSIZE - 1);
		tpanel.buffer[tpanel.tail] = (tpanel.buffer[tpanel.tail] & 0xffffff00) |
			0x20000000 | errcnt;
#ifdef DEBUG_TIMING_PARAMS
		tpanel.dropped_count++;
#endif
	}
	tpanel.lostflag = 0;
}

static void tpanel_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	unsigned int cschange = 0, data1 = 0, data2 = 0;
	unsigned short intreg, pendown;

	*VR41XX_PIUCNTREG = 0x0326;	// sometimes bits get reset in this reg
	barrier();			// so we just reinitialize it

	while ((intreg = (*VR41XX_PIUINTREG & 0x807d)) || cschange) {

		pendown = *VR41XX_PIUCNTREG & 0x2000;
		if (tpanel.lastcontact != (pendown >> 13)) {
			cschange = 1;
#ifdef CONFIG_CPU_VR4181
		// VR4181 has extra clock mask bits, but they apply to AIU also
// MFK: so add in coordination with AIU...
		// these extra bits only have to be on for actual scanning
		// not for wait-for-pen-touch
			if (pendown) {
				*VR41XX_CMUCLKMSK |= VR41XX_CMUCLKMSK_MSKADUPCLK |
				                     VR41XX_CMUCLKMSK_MSKADU18M;
			} else {
				*VR41XX_CMUCLKMSK &= ~(VR41XX_CMUCLKMSK_MSKADUPCLK |
				                       VR41XX_CMUCLKMSK_MSKADU18M);
			}
#endif
		}
		tpanel.lastcontact = pendown >> 13;

		if (intreg & VR41XX_PIUINTREG_PENCHG) {
			*VR41XX_PIUINTREG = VR41XX_PIUINTREG_PENCHG;
		}

			// PADDLOST interrupt: datapoint lost, just report it
			// (after processing data and pen-down)
		if (((intreg & (VR41XX_PIUINTREG_PADDLOST | VR41XX_PIUINTREG_PADPAGE0 |
		    VR41XX_PIUINTREG_PADPAGE1)) == VR41XX_PIUINTREG_PADDLOST) &&
		    !(cschange && pendown)) {

			*VR41XX_PIUINTREG = VR41XX_PIUINTREG_PADDLOST;
			barrier();
			*VR41XX_PIUCNTREG = 0x0022;
			barrier();

			pendown = *VR41XX_PIUCNTREG & 0x2000;
			if (tpanel.lastcontact != (pendown >> 13))
				cschange = 1;
			tpanel.lastcontact = pendown >> 13;

			if (pendown) {
				*VR41XX_PIUCNTREG = 0x0062;
				barrier();
				*VR41XX_PIUCNTREG = 0x0066;	// force manual scan
			} else {
				*VR41XX_PIUCNTREG = 0x0322;
				barrier();
				*VR41XX_PIUCNTREG = 0x0326;	// resume autoscan
			}

			tpanel.lostflag = 1;
#ifdef DEBUG_TIMING_PARAMS
			if ((intreg & (VR41XX_PIUINTREG_PADPAGE1 | VR41XX_PIUINTREG_PADPAGE0))
				 == (VR41XX_PIUINTREG_PADPAGE1 | VR41XX_PIUINTREG_PADPAGE0))
				tpanel.overflow_count++;
			else
				tpanel.incompletescan_count++;
#endif
		}

			// Panel contact state change interrupt
			// If pen-up, do data ints first
		if (cschange && (pendown || !(intreg & (VR41XX_PIUINTREG_PADPAGE0 |
		    VR41XX_PIUINTREG_PADPAGE1)))) {
			data1 = (pendown | 0x4000) << 17;
			tpanel_add_data(data1, 0);
			cschange = 0;
		}

			// PADCMD and PADADP shouldn't happen
		if (intreg & (VR41XX_PIUINTREG_PADCMD | VR41XX_PIUINTREG_PADADP)) {
			*VR41XX_PIUINTREG = VR41XX_PIUINTREG_PADCMD |
					    VR41XX_PIUINTREG_PADADP;
#ifdef DEBUG_TIMING_PARAMS
			tpanel.unhandledint_count++;
#endif
		}

			// Data buffer interrupt
		if (intreg & VR41XX_PIUINTREG_PADPAGE0) {
#ifdef DEBUG_TIMING_PARAMS
			if (intreg & VR41XX_PIUINTREG_PADPAGE1)
				tpanel.dualpagevalid_count++;
#endif
			if ((intreg & (VR41XX_PIUINTREG_PADPAGE1 | VR41XX_PIUINTREG_OVP))
				 != (VR41XX_PIUINTREG_PADPAGE1 | VR41XX_PIUINTREG_OVP)) {
				data1 = 0x40000000 |
				        ((*VR41XX_PIUPB01REG & 0x3ff) << 18) |
					((*VR41XX_PIUPB00REG & 0x3ff) << 8);
				data2 = ( *VR41XX_PIUPB03REG << 20) |
					((*VR41XX_PIUPB02REG & 0x3ff) << 10) |
					( *VR41XX_PIUPB04REG & 0x3ff);
				tpanel_add_data(data1, data2);

				*VR41XX_PIUINTREG = VR41XX_PIUINTREG_PADPAGE0;
			}
		}
		if (intreg & VR41XX_PIUINTREG_PADPAGE1) {
			data1 = 0x40000000 |
			        ((*VR41XX_PIUPB11REG & 0x3ff) << 18) |
				((*VR41XX_PIUPB10REG & 0x3ff) << 8);
			data2 = ( *VR41XX_PIUPB13REG << 20) |
				((*VR41XX_PIUPB12REG & 0x3ff) << 10) |
				( *VR41XX_PIUPB14REG & 0x3ff);
			tpanel_add_data(data1, data2);

			*VR41XX_PIUINTREG = VR41XX_PIUINTREG_PADPAGE1;
		}
	}

	if (data1) {
		add_mouse_randomness(data1);
		if (data2)
			add_mouse_randomness(data2);
		wake_up_interruptible(&tpanel.wait);

		if (tpanel.fasyncptr)
			kill_fasync(&tpanel.fasyncptr, SIGIO, POLL_IN);
	}

}

static int fasync_tpanel(int fd, struct file *filp, int on)
{
	int retval;

	retval = fasync_helper(fd, filp, on, &tpanel.fasyncptr);
	if (retval < 0)
		return retval;
	return 0;
}

static int close_tpanel(struct inode * inode, struct file * file)
{
	fasync_tpanel(-1, file, 0);
	if (--tpanel.active)
		return 0;
		// set for standby
	*VR41XX_MPIUINTREG = 0;
	*VR41XX_PIUCNTREG = 0;
	free_irq(VR41XX_IRQ_PIU, NULL);
	cli();
	*VR41XX_CMUCLKMSK &= ~VR41XX_CMUCLKMSK_MSKPIU;
#ifdef CONFIG_CPU_VR4181
// MFK: coordinate with AIU driver
	*VR41XX_CMUCLKMSK &= ~VR41XX_CMUCLKMSK_MSKADUPCLK | VR41XX_CMUCLKMSK_MSKADU18M;
#endif
	sti();
	MOD_DEC_USE_COUNT;
	return 0;
}

static int open_tpanel(struct inode * inode, struct file * file)
{
	if (tpanel.active++)
		return 0;
	cli();
	*VR41XX_CMUCLKMSK |= VR41XX_CMUCLKMSK_MSKPIU;
	sti();
	*VR41XX_MPIUINTREG = 0;
	barrier();
	tpanel.tail = tpanel.head;
	tpanel.lastcontact = 0;

			// set up for autoscan
	*VR41XX_PIUCNTREG = 0x0322;	// autoscan, sequence suspended
	*VR41XX_PIUSIVLREG = 333;	// set interval to .01 sec default
	*VR41XX_PIUSTBLREG = 16;
	barrier();
	*VR41XX_MPIUINTREG = 0x007d;
	*VR41XX_PIUINTREG = 0x007d;	// clear any pending ints
	barrier();

	if (request_irq(VR41XX_IRQ_PIU, tpanel_interrupt, SA_SHIRQ, "vr41xxtpanel", NULL)) {
		tpanel.active--;
		return -EBUSY;
	}
	MOD_INC_USE_COUNT;
	barrier();

	*VR41XX_PIUCNTREG = 0x0326;	// autoscan, sequence enabled
	return 0;
}

/*
 *  Read touch panel data.
 *
 *  Data format:
 *  unsigned short status: bit 15 = xyz data valid (0 means contact state only)
 *                         bit 14 = pen contact state (1 means contact)
 *                         bit 13 = soft data lost flag: if this is 1, this data
 *                                  is valid (if bit 15 is 1), but data was lost
 *                                  between this point and the previous point
 *                                  due to a buffer overrun in the driver
 *                         bit 12 = hard data lost flag: if this is 1, this point
 *                                  is valid (if bit 15 is 1), but data was lost
 *                                  between this point and the previous point
 *                                  due to hardware error
 *                         bits 11-8 = reserved
 *                         bits 7-0 = count of how many data packet lost, if hard
 *                                    or soft error is falgged
 *  unsigned short x+ raw data (if status:15 = 1)
 *  unsigned short x- raw data (if status:15 = 1)
 *  unsigned short y+ raw data (if status:15 = 1)
 *  unsigned short y- raw data (if status:15 = 1)
 *  unsigned short z (pressure) raw data (if status:15 = 1)
 *
 *  x+, x-, y+, and y- are limited to range 0-1023 for this hardware.  Each +/-
 *  pair is somewhat redundant: the + value can be used as is, but using the
 *  difference (eg. (x+) - (x-)) will produce a more accurate result.  Further
 *  manipulation and/or statistical analysis may be required for best accuracy.
 *
 *  No calibration is done on the driver side, that is expected to be done on
 *  the user side.
 */

static ssize_t read_tpanel(struct file * file,
	char * buffer, size_t count, loff_t *ppos)
{
	unsigned int data1, data2;
	size_t retcnt = 0;

	if (count < 12)
		return -EINVAL;

		// We need to access the circular buffer atomic to
		// anything else that will read or write it
	cli();

	while (tpanel.head == tpanel.tail) {
		if (file->f_flags & O_NONBLOCK) {
			sti();
			return -EAGAIN;
		}
		interruptible_sleep_on(&tpanel.wait);
		// sleep_on will sti for us
		if (signal_pending(current))
			return -ERESTARTSYS;
		cli();
	}
	do {
		data1 = tpanel.buffer[tpanel.tail++];
		data2 = tpanel.buffer[tpanel.tail++];
		tpanel.tail &= BUFFSIZE - 1;

		sti();

		if (!access_ok(VERIFY_WRITE, buffer + retcnt, 12))
			return -EFAULT;

		__put_user((((data1 >> 16) & 0xf000) | (data1 & 0x00ff)) ^ 0x8000,
			   (short*)(buffer + retcnt));
		__put_user((data1 >> 18) & 0x03ff, (short*)(buffer + retcnt + 2));
		__put_user((data1 >> 8) & 0x03ff, (short*)(buffer + retcnt + 4));
		__put_user((data2 >> 20) & 0x03ff, (short*)(buffer + retcnt + 6));
		__put_user((data2 >> 10) & 0x03ff, (short*)(buffer + retcnt + 8));
		__put_user(data2 & 0x03ff, (short*)(buffer + retcnt + 10));

		retcnt += 12;
		if (retcnt > count - 12)
			return retcnt;

		cli();
	} while (tpanel.head != tpanel.tail);

	sti();

	return retcnt;
}

static unsigned int poll_tpanel(struct file *file, poll_table * wait)
{
	int comp;

	poll_wait(file, &tpanel.wait, wait);
	cli();
	comp = (tpanel.head != tpanel.tail);
	sti();
	if (comp)
		return POLLIN | POLLRDNORM;
	return 0;
}

static int ioctl_tpanel(struct inode * inode, struct file * file,
	unsigned int cmd, unsigned long arg)
{
	struct scanparam *sparm;
	unsigned int i1, i2;

	sparm = (struct scanparam *)arg;

	switch (cmd) {
	case TPGETSCANPARM:
		i1 = *VR41XX_PIUSIVLREG * 30;	// PIUSIVLREG is in 30us units
		if (put_user(i1, &sparm->interval))
			return -EFAULT;
		i2 = *VR41XX_PIUSTBLREG * 30;	// PIUSTBLREG is in 30us units
		if (put_user(i2, &sparm->settletime))
			return -EFAULT;
		return 0;
	case TPSETSCANPARM:
		if (get_user(i1, &sparm->interval))
			return -EFAULT;
		i1 = (i1 + 15) / 30;
		if (i1 > 0x07ff)
			return -EINVAL;
		if (get_user(i2, &sparm->settletime))
			return -EFAULT;
		i2 = (i2 + 15) / 30;
		if (i2 > 0x003f)
			return -EINVAL;
		*VR41XX_PIUSIVLREG = (unsigned short)i1;
		*VR41XX_PIUSTBLREG = (unsigned short)i2;
		__put_user(i1 * 30, &sparm->interval);
		__put_user(i2 * 30, &sparm->settletime);
		return 0;
	default:
		return -EINVAL;
	}
}

struct file_operations vr41xx_tpanel_fops = {
	read:		read_tpanel,
	poll:		poll_tpanel,
	ioctl:		ioctl_tpanel,
	open:		open_tpanel,
	release:	close_tpanel,
	fasync:		fasync_tpanel,
};

static struct miscdevice vr41xx_tpanel = {
	VR41XX_TPANEL_MINOR, "vr41xxtpanel", &vr41xx_tpanel_fops
};

#ifdef DEBUG_TIMING_PARAMS
static int get_piuinfo(char *page, char **start, off_t off,
	int count, int *eof, void *data)
{
	int len;

	// return lots of nifty diagnostic info
	// buffer is PAGE_SIZE long, we're OK for all page sizes VR41xx supports
	len = sprintf(page,
		"piucntreg:            0x%04hx\n"
		"piusivlreg:           0x%04hx\n"
		"piustblreg:           0x%04hx\n"
		"orig piusivlreg:      0x%04hx\n"
		"orig piustblreg:      0x%04hx\n"
		"dropped data count:   %d\n"
		"page overflow count:  %d\n"
		"bad scan count:       %d\n"
		"dual page count:      %d\n"
		"unhandled int count:  %d\n",
		*VR41XX_PIUCNTREG, *VR41XX_PIUSIVLREG, *VR41XX_PIUSTBLREG,
		tpanel.orig_scanint, tpanel.orig_settletime, tpanel.dropped_count,
		tpanel.overflow_count, tpanel.incompletescan_count,
		tpanel.dualpagevalid_count, tpanel.unhandledint_count);

	if (len <= off+count) *eof = 1;
	*start = page + off;
	len -= off;
	if (len > count) len = count;
	if (len < 0) len = 0;
	return len;
}
#endif

#ifdef CONFIG_PM
static int pm_tpanel_request(struct pm_dev *dev, pm_request_t rqst, void *data)
{
	static unsigned short piu_state[4];

	switch (rqst) {
	case PM_SUSPEND:
		piu_state[0] = *VR41XX_PIUCNTREG;
		piu_state[1] = *VR41XX_PIUSIVLREG;
		piu_state[2] = *VR41XX_PIUSTBLREG;
		piu_state[3] = *VR41XX_MPIUINTREG;
		break;
	case PM_RESUME:
		*VR41XX_PIUCNTREG = piu_state[0];
		*VR41XX_PIUSIVLREG = piu_state[1];
		*VR41XX_PIUSTBLREG = piu_state[2];
		*VR41XX_PIUINTREG = ~0;
		barrier();
		*VR41XX_MPIUINTREG = piu_state[3];
		break;
	}
	return 0;
}
#endif

int __init vr41xx_tpanel_init(void)
{
#ifdef DEBUG_TIMING_PARAMS
	*VR41XX_CMUCLKMSK |= VR41XX_CMUCLKMSK_MSKPIU;
	barrier();

	tpanel.orig_scanint = *VR41XX_PIUSIVLREG;
	tpanel.orig_settletime = *VR41XX_PIUSTBLREG;
	tpanel.dropped_count = 0;
	tpanel.overflow_count = 0;
	tpanel.incompletescan_count = 0;
	tpanel.dualpagevalid_count = 0;
	tpanel.unhandledint_count = 0;

	create_proc_read_entry("piuinfo", 0, NULL, get_piuinfo, NULL);
#endif
	// power down outputs and reset the PIU
	*VR41XX_PIUCNTREG = VR41XX_PIUCNTREG_PIUPWR;

	tpanel.active = 0;
	tpanel.head = tpanel.tail = 0;
	init_waitqueue_head(&tpanel.wait);
	tpanel.fasyncptr = NULL;
	printk(KERN_INFO "VR41xx touch panel initialized, using IRQ %d version "TPANEL_VERSION"\n",
	       VR41XX_IRQ_PIU);
	misc_register(&vr41xx_tpanel);
#ifdef CONFIG_PM
	pm_register(PM_ISA_DEV, PM_SYS_UNKNOWN, pm_tpanel_request);
#endif

	// clocks remained masked until the touch panel device is opened
	*VR41XX_CMUCLKMSK &= ~VR41XX_CMUCLKMSK_MSKPIU;
#ifdef CONFIG_CPU_VR4181
// MFK: coordinate with AIU
	*VR41XX_CMUCLKMSK &= ~VR41XX_CMUCLKMSK_MSKADUPCLK | VR41XX_CMUCLKMSK_MSKADU18M;
#endif
	return 0;
}

#ifdef MODULE
int init_module(void)
{
	return vr41xx_tpanel_init();
}

void cleanup_module(void)
{
	misc_deregister(&vr41xx_tpanel);
}
#endif

