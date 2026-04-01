/* 
 * GPIO button functions for VR41xx button input driver
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

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/timer.h>
#include <linux/buttons.h>

#include <asm/vr41xx-platdep.h>

extern void add_button_data(unsigned char);


#ifdef CONFIG_CPU_VR4181
typedef u16 btn_mask_t;
#define NUM_GPIO_IRQS 16
#else
#ifdef CONFIG_HAS_VRC4173
typedef u64 btn_mask_t;
#define NUM_GPIO_IRQS 52
#else
typedef u32 btn_mask_t;
#define NUM_GPIO_IRQS 32
#endif
#endif

#ifdef GPIO_BTN_MAP
unsigned short gpio_btn_map[NUM_GPIO_IRQS] = GPIO_BTN_MAP;
#else
#warning "No GPIO button map defined, using default blank map"
// Implicit 0 initializer:
unsigned short gpio_btn_map[NUM_GPIO_IRQS];
#endif

struct gpiobtns_device {
	struct timer_list   btn_timer;
    btn_mask_t          btn_timer_data;
	btn_mask_t          btn_state;
	btn_mask_t          btn_bounce;
	btn_mask_t          pending_bits;
	unsigned char       press_low;
};

#ifdef GPIO_BTN_PRESS_LOW
static struct gpiobtns_device gpiob = { { }, 0, 0, 0, 0, 0x80 };
#else
static struct gpiobtns_device gpiob;
#endif

#ifdef CONFIG_HAS_VRC4173
extern void vrc4173_init_buttons(void);
extern void vrc4173_close_buttons(void);
extern btn_mask_t vrc4173_gpio_state(void);
#endif

static void debounce_timer_event(unsigned long data)
{
	btn_mask_t curstate, same, mask;
	int i;
	long flags;

	// This is fairly nasty, but we need to make sure no button interrupt
	// happens while we're tweaking the debounce bits
	save_and_cli(flags);

	// reread data rather than trust passed arg, since int handler may
	// have added bits
	mask = gpiob.btn_timer_data;
#ifdef CONFIG_CPU_VR4181
	curstate = *VR41XX_GPDATLREG;
#else
	curstate = *VR41XX_GIUPIODL | ((btn_mask_t)*VR41XX_GIUPIODH << 16);
#ifdef CONFIG_HAS_VRC4173
	curstate |= vrc4173_gpio_state();
#endif
#endif
	gpiob.btn_state ^= mask;
	same = (curstate ^ gpiob.btn_state) & mask;
	gpiob.btn_bounce &= ~(same ^ mask);
	// if state is same as before transition detected, we either missed a
	// transition, or button switched state for real during debounce period
	if (same)
		for (i = 0; i < NUM_GPIO_IRQS; i++)
		    if (same & ((btn_mask_t)1 << i))
				add_button_data((((gpiob.btn_state & ((btn_mask_t)1 << i)) ?
						0 : 0x80) ^ gpiob.press_low) | i);
	gpiob.btn_timer_data = gpiob.pending_bits | same;
	gpiob.pending_bits = 0;
	if (gpiob.btn_timer_data) {
		gpiob.btn_timer.expires = jiffies + (HZ + 99)/100;
		add_timer(&gpiob.btn_timer);
	}

	restore_flags(flags);
}

void gpio_handle_btn_interrupt(int gpio)
{
	btn_mask_t gpiobit;

	// shouldn't get a non-button irq, but just in case...
	if (!gpio_btn_map[gpio])
		return;

	gpiobit = (btn_mask_t)1 << gpio;
	if (gpiob.btn_bounce & gpiobit)
		return;

	// add data as opposite of previous state, rather than current pin state
	add_button_data((((gpiob.btn_state & gpiobit) ? 0 : 0x80) ^
			 gpiob.press_low)| gpio);
	gpiob.btn_bounce |= gpiobit;
	// debounce for at least 10ms past end of current timer tick
	// if timer event not active yet, just activate it for this bit
	if (!gpiob.btn_timer_data) {
		gpiob.btn_timer.expires = jiffies + (HZ + 99)/100 + 1;
		gpiob.btn_timer_data = gpiobit;
		add_timer(&gpiob.btn_timer);
		return;
	}
	// if timer event already active at right time, add this bit
	if (time_after_eq(gpiob.btn_timer.expires, jiffies + (HZ + 99)/100 + 1)) {
		gpiob.btn_timer_data |= gpiobit;
		return;
	}
	// otherwise, wait until the timer event to (re)activate for this bit
	gpiob.pending_bits |= gpiobit;
}

static void gpio_btn_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	unsigned short bit;
	int gpio = irq - 40;

	// flip active level for interrupt to opposite of current pin state
#ifdef CONFIG_CPU_VR4181
	bit = 1 << ((gpio & 7) * 2);
	if (*VR41XX_GPDATLREG & (1 << gpio))
		*(gpio < 8 ? VR41XX_GPINTTYPL : VR41XX_GPINTTYPH) &= ~bit;
	else
		*(gpio < 8 ? VR41XX_GPINTTYPL : VR41XX_GPINTTYPH) |= bit;
#else
	bit = 1 << (gpio & 15);
	if (*(gpio < 16 ? VR41XX_GIUPIODL : VR41XX_GIUPIODH) & bit)
		*(gpio < 16 ? VR41XX_GIUINTALSELL : VR41XX_GIUINTALSELH) &= ~bit;
	else
		*(gpio < 16 ? VR41XX_GIUINTALSELL : VR41XX_GIUINTALSELH) |= bit;
#endif
	gpio_handle_btn_interrupt(gpio);
}

void open_gpio_buttons(void)
{
	int i;
	unsigned short bits;

	init_timer(&gpiob.btn_timer);
	gpiob.btn_timer.function = &debounce_timer_event;

	for (i = 0; i < NUM_GPIO_IRQS; i++) {
		if (!(gpio_btn_map[i]))
			continue;
#ifdef CONFIG_CPU_VR4181
		bits = 0x0003 << ((i & 7) * 2);
		if (*(i < 8 ? VR41XX_GPMD0REG : VR41XX_GPMD1REG) & bits) {
			gpio_btn_map[i] = 0;
			continue;
		}
		// see README.GIU for explanation of why cli/sti
		cli();
		*(i < 8 ? VR41XX_GPINTTYPL : VR41XX_GPINTTYPH) |= bits;
#else
#ifdef CONFIG_HAS_VRC4173
		if (i >= 32)
		    break;
#endif
		bits = 1 << (i & 15);
		if (*(i < 16 ? VR41XX_GIUIOSELL : VR41XX_GIUIOSELH) & bits) {
			gpio_btn_map[i] = 0;
			continue;
		}
		// see README.GIU for explanation of why cli/sti
		cli();
		*(i < 16 ? VR41XX_GIUINTTYPL : VR41XX_GIUINTTYPH) &= ~bits;
		*(i < 16 ? VR41XX_GIUINTALSELL : VR41XX_GIUINTALSELH) |= bits;
		*(i < 16 ? VR41XX_GIUINTHTSELL : VR41XX_GIUINTHTSELH) |= bits;
#endif
		sti();

		if (request_irq(i + 40, gpio_btn_interrupt, SA_SAMPLE_RANDOM |
		    SA_INTERRUPT, "vrbuttons", NULL))
			gpio_btn_map[i] = 0;
	}
#ifdef CONFIG_HAS_VRC4173
	vrc4173_init_buttons();
#endif
}

void close_gpio_buttons(void)
{
	int i;

	for (i = 0; i < NUM_GPIO_IRQS; i++)
		if (gpio_btn_map[i])
			free_irq(i + 40, NULL);
	gpiob.pending_bits = 0;
	del_timer(&gpiob.btn_timer);
	gpiob.btn_state = 0;
	gpiob.btn_bounce = 0;
#ifdef CONFIG_HAS_VRC4173
	vrc4173_close_buttons();
#endif
}

