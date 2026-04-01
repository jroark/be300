/*
 *
 * Alchemy Semi Hyd1100 board specific pcmcia routines.
 *
 * Copyright 2002 MontaVista Software Inc.
 * Author: MontaVista Software, Inc.
 *         	ppopov@mvista.com or source@mvista.com
 *
 * ########################################################################
 *
 *  This program is free software; you can distribute it and/or modify it
 *  under the terms of the GNU General Public License (Version 2) as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place - Suite 330, Boston MA 02111-1307, USA.
 *
 * ########################################################################
 *
 * 
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/config.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/tqueue.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/version.h>
#include <linux/types.h>

#include <pcmcia/version.h>
#include <pcmcia/cs_types.h>
#include <pcmcia/cs.h>
#include <pcmcia/ss.h>
#include <pcmcia/bulkmem.h>
#include <pcmcia/cistpl.h>
#include <pcmcia/bus_ops.h>
#include "cs_internal.h"

#include <asm/io.h>
#include <asm/irq.h>
#include <asm/system.h>

#include <asm/au1000.h>
#include <asm/au1000_pcmcia.h>
#include <asm/hyd1100.h>


static int hyd1100_pcmcia_init(struct pcmcia_init *init)
{
	return 1; // one slot
}

static int hyd1100_pcmcia_shutdown(void)
{
	return 0; // nothing to do
}

static int 
hyd1100_pcmcia_socket_state(unsigned sock, struct pcmcia_state *state)
{
	u32 inserted;
	inserted = (readl(SYS_PINSTATERD) & (1<<22)) ? 0 : 1;

	state->ready = 0;
	state->vs_Xv = 0;
	state->vs_3v = 0;
	state->detect = 0;

	if (inserted)
	{
		state->vs_3v=1;
		state->detect = 1;
		state->ready = 1;
	}

	state->bvd1=1;
	state->bvd2=1;
	state->wrprot=0; 
	return 1;
}


static int hyd1100_pcmcia_get_irq_info(struct pcmcia_irq_info *info)
{

	if(info->sock > PCMCIA_MAX_SOCK) return -1;

	if(info->sock == 0)
		info->irq = AU1000_GPIO_21;
	else 
		info->irq = -1;

	return 0;
}


static int 
hyd1100_pcmcia_configure_socket(const struct pcmcia_configure *configure)
{
	if(configure->sock > PCMCIA_MAX_SOCK) return -1;

	au_sync_delay(300);

	if (!configure->reset) {
		// de-assert reset
		writel(0x00010000, 0xb1700008);
		au_sync_delay(100);
	}
	else {
		// assert reset
		writel(0x00010001, 0xb1700008);
		au_sync_delay(300);
	}
	return 0;
}

void hyd1100_init()
{
	struct pcmcia_configure conf;
	hyd1100_pcmcia_init(NULL);

	conf.sock = 0;
	conf.vcc = 50;
	conf.vpp = 50;
	conf.reset = 1;
	hyd1100_pcmcia_configure_socket(&conf);
}

struct pcmcia_low_level hyd1100_pcmcia_ops = { 
	hyd1100_pcmcia_init,
	hyd1100_pcmcia_shutdown,
	hyd1100_pcmcia_socket_state,
	hyd1100_pcmcia_get_irq_info,
	hyd1100_pcmcia_configure_socket
};
