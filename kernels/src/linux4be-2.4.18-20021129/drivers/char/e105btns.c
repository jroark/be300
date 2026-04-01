/* 
 * Casio E-10x/50x input button (sub)driver
 *
 * Original driver by Robert Coie, modified and integrated into main VR41xx
 * buttons driver by Michael Klar.
 *
 * Copyright (c) 1999, Robert Coie <rac@intrigue.com>
 * Copyright (c) 2000, Michael Klar, mfklar@ponymail.com
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/ioport.h>
#include <linux/buttons.h>

#include <asm/io.h>
#include <asm/vr41xx.h>

extern void add_button_data(unsigned char);

#define E105_BUTTONS_GPIO	9
#define E105_BUTTONS_IRQ	VR41XX_IRQ_GPIO(E105_BUTTONS_GPIO)
#define E105_BUTTONS_MASK	(1 << E105_BUTTONS_GPIO)
#define E105_BUTTONS_PORT	0xA018
#define E105_BUTTONS_DEV_ID	((void *) 0x45357878)	/* 'E5xx' */

const unsigned short e105_btn_map[] = { BTN_UP, BTN_DOWN, BTN_ACTION,
		 BTN_EXIT, BTN_NORTH, BTN_SOUTH, BTN_EAST, BTN_WEST };

struct e105_buttons_device_t {
	struct timer_list	timer;
	u8			status;
	volatile char		bounce;
};

static struct e105_buttons_device_t buttons;

// this is only called from int handler if no timer active, so not reentrant
//
static void
process_e105_buttons(void)
{
	u8	new_status, diff_status;
	int	i;

	diff_status = buttons.status;
	buttons.status = new_status = inb(E105_BUTTONS_PORT);
	diff_status ^= new_status;

	if (diff_status == 0)
		return;
	for (i = 0; i < 8; i++)
		if (diff_status & (1 << i))
			add_button_data((new_status & (1 << i) ? 0 : 0x80) |
		                        (i + 0x20));
}

static void
e105_button_timer(unsigned long data)
{
	// if bouncing, just unmark bounce and go on, will handle data
	// next timer tick
	if (buttons.bounce) {
		buttons.bounce = 0;
	} else {
		process_e105_buttons();
		if (!buttons.status) {
			unsigned long flags;

			// handle potential race with int handler:
			save_and_cli(flags);
			if (!buttons.bounce) {
				buttons.timer.data = 0;
				restore_flags(flags);
				return;
			}
			restore_flags(flags);
		}
	}
	buttons.timer.expires = jiffies + (HZ + 99)/100;
	add_timer(&buttons.timer);
}

static void
e105_buttons_interrupt(int irq, void * dev_id, struct pt_regs * regs)
{
	// flip active level for interrupt to opposite of current pin state
	if (*VR41XX_GIUPIODL & E105_BUTTONS_MASK)
		*VR41XX_GIUINTALSELL &= ~E105_BUTTONS_MASK;
	else
		*VR41XX_GIUINTALSELL |= E105_BUTTONS_MASK;

	// if timer not already active, activate it
	// this is only case for first button press if multiple buttons
	// pressed simulateously
	if (!buttons.timer.data) {
		buttons.timer.expires = jiffies + (HZ + 99)/100 + 1;
		buttons.timer.data = 1;
		add_timer(&buttons.timer);
		process_e105_buttons();
	}
	// otherwise just mark as (still) bouncing
	buttons.bounce = 1;
}

void open_e105_buttons(void)
{
	if (check_region(E105_BUTTONS_PORT, 1))
		return;

	init_timer(&buttons.timer);
	buttons.timer.function = &e105_button_timer;

	// see README.GIU for explaination of why cli/sti
	cli();
	*VR41XX_GIUINTTYPL   &= ~E105_BUTTONS_MASK;
	*VR41XX_GIUINTALSELL |=  E105_BUTTONS_MASK;
	sti();

	if (request_irq(E105_BUTTONS_IRQ, e105_buttons_interrupt,
	                SA_SHIRQ | SA_SAMPLE_RANDOM | SA_INTERRUPT,
	                "E105buttons", E105_BUTTONS_DEV_ID));
		return;
	request_region(E105_BUTTONS_PORT, 1, "E105buttons");
}

void close_e105_buttons(void)
{
	free_irq(E105_BUTTONS_IRQ, E105_BUTTONS_DEV_ID);
	del_timer_sync(&buttons.timer);
	buttons.status = 0;
	buttons.bounce = 0;
	release_region(E105_BUTTONS_PORT, 1);
}

