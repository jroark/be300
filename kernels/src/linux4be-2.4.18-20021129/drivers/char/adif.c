/*
 * linux/arch/mips/vr41xx/adif.c
 *
 * VR41xx Lvl A/D driver
 *
 * Parts Copyright (C) 2001 Chris AtLee <catlee@canada.com>
 * Parts Copyright (C) 2001 Paul Jimenez <pj@agendacomputing.com>
 * Parts Copyright (C) 2001 Alexandre d'Alton <alexandre_dalt@yahoo.com>
 * 
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/major.h>
#include <linux/sched.h>

#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/irq.h>
#include <asm/vr41xx.h>

#define VR41XX_ADIF_MINOR 15

static wait_queue_head_t wait;

static void
adif_irq_handler (int irq, void *dev_id, struct pt_regs *regs)
{
  wake_up_interruptible (&wait);
}


static int
adif_open (struct inode *inode, struct file *file)
{
  /* Readonly, sure they can all be open */
  return 0;
}


static int
adif_close (struct inode *inode, struct file *file)
{
  return 0;
}

static ssize_t
adif_read (struct file *file, char *buf, size_t count, loff_t * offset)
{
  int original_stable;
  unsigned short adin[4];
  ssize_t retval;
  unsigned long flags;
  unsigned int j;

  if (count < 4 * sizeof (short))
	{
	  printk ("adif: Invalid count(%d) specified (must be at least %d)\n",
			  count, 4 * sizeof (short));
	  return 0;
	}

	/*** get battery readings here */
	/** wait til we get into one of Standby, WaitPenTouch, or Interval state per
           VR4181.pdf 19.4 */
  /* If the pen is down, we can't use the PIU, so we wait. */
  cli();
  while(*VR41XX_PIUCNTREG & 0x2000){
    sti();
    j = jiffies + 100;
    while(jiffies < j)
      schedule();
  }
  
  save_and_cli (flags);
  /* We need to activate the PIU clocks. */
  *VR41XX_CMUCLKMSK |= (VR41XX_CMUCLKMSK_MSKKIU|
						VR41XX_CMUCLKMSK_MSKSIU);
  barrier ();

  /* All these step are probably not needed, but they were for debugging */
  *VR41XX_PIUCNTREG = 1;

  *VR41XX_PIUCNTREG = 0x2;		/* Power up the PIU */

#define PIU_IN_STANDBY ((*VR41XX_PIUCNTREG & 0x1c00) == 0x0400)

  while (!PIU_IN_STANDBY)
	*VR41XX_PIUCNTREG &= ~0x2;	/* Go in standby state */
  barrier ();
  *VR41XX_PIUAMSKREG = 0xFF0F;	/* Mask ADIN[0-2] & MIC pins */
  barrier ();

  restore_flags (flags);

  /* set irq */
  /* When we share IRQ we have to differentiate between two handlers
     for the same IRQ, that's why we put the address ow the wait queue
     as last argument to request_irq. */
  if (retval = request_irq (VR41XX_IRQ_PIU, adif_irq_handler, SA_SHIRQ,
							"vr41xx_battery_level", &wait))
	{
	  /* restore PIUSTBLREG */
//                *VR41XX_PIUSTBLREG = original_stable;
	  printk ("adif: unable to request_irq (%d)\n", retval);
	  return -retval;
	}
  barrier ();

  save_and_cli (flags);
  /* start the process */
  *VR41XX_PIUASCNREG = 0x1;
  barrier ();
  *VR41XX_PIUCNTREG |= 0x0004;
  barrier ();
  restore_flags (flags);

  /* sleep until the value is ready */
  interruptible_sleep_on (&wait);

  save_and_cli (flags);
  *VR41XX_PIUAMSKREG = 0xFFF0;
  *VR41XX_PIUASCNREG = 0x2;
  barrier ();

  /* read values */
  adin[0] = *VR41XX_PIUAB0REG;
  adin[1] = *VR41XX_PIUAB1REG;
  adin[2] = *VR41XX_PIUAB2REG;
  adin[3] = *VR41XX_PIUAB3REG;

  /* Reset the PIU Controler in its default state */
  *VR41XX_PIUCNTREG = 1;
  barrier();
  *VR41XX_PIUCNTREG = 0x2;		/* Power up the PIU */
  barrier();
  while (!PIU_IN_STANDBY)
	*VR41XX_PIUCNTREG &= ~0x2;	/* Go in standby state */
  *VR41XX_PIUCNTREG = 0x0322;
  barrier ();
  *VR41XX_PIUSIVLREG = 333;		// set interval to .01 sec default
  *VR41XX_PIUSTBLREG = 0x10;
  barrier ();
  *VR41XX_MPIUINTREG = 0x007d;
  barrier ();
  *VR41XX_PIUCNTREG = 0x0326;	// resume autoscan
  barrier ();
  /* Shut down the clocks. */
  *VR41XX_CMUCLKMSK &= ~(VR41XX_CMUCLKMSK_MSKKIU |
						 VR41XX_CMUCLKMSK_MSKSIU);
  restore_flags (flags);

  free_irq (VR41XX_IRQ_PIU, &wait);

//  printk ("adif: returning %d, %d, %d, %d\n", adin[0], adin[1], adin[2],
//		  adin[3]);
  if (!access_ok(VERIFY_WRITE, buf, 8))
    return -EFAULT;

  __put_user (adin[0], (short *) (buf));
  __put_user (adin[1], (short *) (buf + 2));
  __put_user (adin[2], (short *) (buf + 4));
  __put_user (adin[3], (short *) (buf + 6));


  return 4 * sizeof (short);
}


static struct file_operations adif_fops;
static struct miscdevice adif_device;


static int
adif_init (void)
{
  int retval;

  init_waitqueue_head (&wait);
  /* NULL out the data structure before filling it */
  memset (&adif_fops, 0, sizeof (struct file_operations));
  adif_fops.read = adif_read;
  adif_fops.open = adif_open;
  adif_fops.release = adif_close;

  /* NULL out the data structure before filling it */
  memset (&adif_device, 0, sizeof (struct miscdevice));
  adif_device.minor = VR41XX_ADIF_MINOR;
  adif_device.name = "adif";
  adif_device.fops = &adif_fops;

  /* register it */
  retval = misc_register (&adif_device);

  if (retval < 0)
	{
	  printk ("Failed to register A/D interface driver\n");
	  return retval;
	}
  printk ("A/D interface driver registered on %i,%i\n", MISC_MAJOR,
		  VR41XX_ADIF_MINOR);

  return 0;
}


static void
adif_exit (void)
{
  if (0 > misc_deregister (&adif_device))
	printk ("Error unregistering A/D interface driver\n");
}

module_init (adif_init);
module_exit (adif_exit);
