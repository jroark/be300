/*
 *
 * BRIEF MODULE DESCRIPTION
 *    PROM library initialization code for the IDT 79S334A and 79EB355
 *    boards, assumes the boot code is IDT/sim.
 *
 * Copyright 2001,2002 MontaVista Software Inc.
 * Author: MontaVista Software, Inc.
 *         	stevel@mvista.com or source@mvista.com
 *
 * This file was derived from Carsten Langgaard's 
 * arch/mips/mips-boards/xx files.
 *
 * Carsten Langgaard, carstenl@mips.com
 * Copyright (C) 1999,2000 MIPS Technologies, Inc.  All rights reserved.
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

#include <linux/kdev_t.h>
#include <linux/major.h>
#include <linux/console.h>
#include <asm/bootinfo.h>
#include <asm/page.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/bootmem.h>
#include <linux/ioport.h>
#include <asm/rc32300/rc32300.h>

char arcs_cmdline[CL_SIZE];

char * __init prom_getcmdline(void)
{
	return &(arcs_cmdline[0]);
}

#ifdef CONFIG_IDT_79EB355
/*
 * NVRAM on the 79EB355 is internal to the DS1511 RTC and uses
 * indexed addressing.
 */
static inline u8 nvreadb(int offset)
{
	writeb((u8)offset, &rtc->nvram_addr);
	return readb(&rtc->nvram_data);
}
#else
static inline u8 nvreadb(int offset)
{
	return *((u8*)KSEG1ADDR(NVRAM_BASE + offset));
}
#endif

static inline u8 env_readb(int env_index)
{
	return nvreadb(NVRAM_ENVSTART_OFF + env_index);
}

/*
 * Parses environment variable strings in NVRAM, copying strings
 * beginning with "bootparm?=" to arcs_cmdline[]. For example,
 *
 *    netaddr=10.0.1.95
 *    bootaddr=10.0.0.139
 *    bootfile=vmlinus
 *    bootparm1=root=/dev/nfs
 *    bootparm2=ip=10.0.1.95
 *
 * is parsed to:
 *
 *	root=/dev/nfs ip=10.0.1.95
 *
 * in arcs_cmdline[].
 */
static void prom_init_cmdline(void)
{
	int env_size, env_index, arcs_index;

	env_index = arcs_index = 0;

	/* stored size is 2 bytes, always big endian order */
	env_size = (int)((nvreadb(NVRAM_ENVSIZE_OFF) << 8) +
			 nvreadb(NVRAM_ENVSIZE_OFF+1));
	
	if (env_size < 0 || env_size > 512)
		return;	/* invalid total env size */

	while (env_index < env_size && arcs_index < sizeof(arcs_cmdline)) {
		char env_str[100];
		int i, arcs_len;
		/*
		  first byte is length of this env variable string,
		  including the length.
		*/
		int env_len = env_readb(env_index);
		int max_len = min(100, env_size - env_index);
		
		if (env_len == 0 || env_len > max_len)
			break;	/* invalid env variable size */
		
		/* copy the env string */
		for (i=0; i<env_len; i++)
			env_str[i] = env_readb(env_index + i);
		
		if (strncmp(&env_str[1], "bootparm", 8) == 0) {
			/*
			  copy to arcs, skipping over length byte and
			  "bootparm?=" string, a total of 11 chars.
			*/
			arcs_len = env_len - 11;

			/* will this string fit in arcs ? */
			if (arcs_index + arcs_len + 1 > sizeof(arcs_cmdline))
				break; /* nope */

			memcpy(&arcs_cmdline[arcs_index],
			       &env_str[11], arcs_len);
			arcs_index += arcs_len;
			/* add a blank between env variables */
			arcs_cmdline[arcs_index++] = ' ';
#ifdef CONFIG_IDT_79EB355
		} else if (strncmp(&env_str[1], "ethaddr", 7) == 0) {
			/* copy to arcs, skipping over length byte */
			arcs_len = env_len - 1;

			/* will this string fit in arcs ? */
			if (arcs_index + arcs_len + 1 > sizeof(arcs_cmdline))
				break; /* nope */

			memcpy(&arcs_cmdline[arcs_index],
			       &env_str[1], arcs_len);
			arcs_index += arcs_len;
			/* add a blank between env variables */
			arcs_cmdline[arcs_index++] = ' ';
#endif
		}
		
		/* increment to next prom env variable */
		env_index += env_len;
	}

	arcs_cmdline[arcs_index] = '\0';
}

extern unsigned long mips_machgroup;
extern unsigned long mips_machtype;

const char *get_system_type(void)
{
#ifdef CONFIG_IDT_79EB355
        return "IDT 79EB355";
#else
        return "IDT 79S334A";
#endif
}

struct resource rc32300_res_ram = {
	"RAM",
	0,
	RAM_SIZE,
	IORESOURCE_MEM
};

int __init prom_init(int argc, char **argv, char **envp, int *prom_vec)
{
	/* set up command line */
	prom_init_cmdline();

	/* set our arch type */
	mips_machgroup = MACH_GROUP_IDT;
#ifdef CONFIG_IDT_79EB355
	mips_machtype = MACH_IDT79EB355;
#else
	mips_machtype = MACH_IDT79S334;
#endif	
	add_memory_region(0,
			  rc32300_res_ram.end - rc32300_res_ram.start,
			  BOOT_MEM_RAM);

	return 0;
}

void prom_free_prom_memory(void)
{
}

