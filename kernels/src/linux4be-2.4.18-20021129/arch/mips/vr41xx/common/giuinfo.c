/*
 * linux/arch/mips/vr41xx/giuinfo.c
 *
 * Shows General Purpose I/O status in /proc/giuinfo
 *
 * Copyright (C) 1999 Bradley D. LaRonde
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 */
/*
 * Changes:
 * Francois Leblanc <francois.leblanc@cev-sa.com> Wed, 04 apr 2002
 * - Add more info view.
 */
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/init.h>
#include <asm/vr41xx.h>

static int get_giuinfo(char *page, char **start, off_t off,
	int count, int *eof, void *data)
{
	int len;

	// return the giu register info
	// buffer is PAGE_SIZE long, we're OK for all page sizes VR41xx supports
#ifdef CONFIG_CPU_VR4181
	len = sprintf(page,
		"GPMD0REG:   0x%04hx\n"
		"GPMD1REG:   0x%04hx\n"
		"GPMD2REG:   0x%04hx\n"
		"GPMD3REG:   0x%04hx\n"
		"GPDATHREG:  0x%04hx\n"
		"GPDATLREG:  0x%04hx\n"
		"GPINTEN:    0x%04hx\n"
		"GPINTMSK:   0x%04hx\n"
		"GPINTTYPH:  0x%04hx\n"
		"GPINTTYPL:  0x%04hx\n"
		"GPINTSTAT:  0x%04hx\n"
		"GPHIBSTH:   0x%04hx\n"
		"GPHIBSTL:   0x%04hx\n"
		"GPSICTL:    0x%04hx\n"
		"KEYEN:      0x%04hx\n"
		"PCS0STRA:   0x%04hx\n"
		"PCS0STPA:   0x%04hx\n"
		"PCS0HIA:    0x%04hx\n"
		"PCS1STRA:   0x%04hx\n"
		"PCS1STPA:   0x%04hx\n"
		"PCS1HIA:    0x%04hx\n"
		"PCSMODE:    0x%04hx\n"
		"LCDGPMODE:  0x%04hx\n"
		"MISCREG0:   0x%04hx\n"
		"MISCREG1:   0x%04hx\n"
		"MISCREG2:   0x%04hx\n"
		"MISCREG3:   0x%04hx\n"
		"MISCREG4:   0x%04hx\n"
		"MISCREG5:   0x%04hx\n"
		"MISCREG6:   0x%04hx\n"
		"MISCREG7:   0x%04hx\n"
		"MISCREG8:   0x%04hx\n"
		"MISCREG9:   0x%04hx\n"
		"MISCREG10:  0x%04hx\n"
		"MISCREG11:  0x%04hx\n"
		"MISCREG12:  0x%04hx\n"
		"MISCREG13:  0x%04hx\n"
		"MISCREG14:  0x%04hx\n"
		"MISCREG15:  0x%04hx\n",
		*VR41XX_GPMD0REG, *VR41XX_GPMD1REG, *VR41XX_GPMD2REG, *VR41XX_GPMD3REG,
		*VR41XX_GPDATHREG, *VR41XX_GPDATLREG, *VR41XX_GPINTEN, *VR41XX_GPINTMSK,
		*VR41XX_GPINTTYPH, *VR41XX_GPINTTYPL, *VR41XX_GPINTSTAT, *VR41XX_GPHIBSTH,
		*VR41XX_GPHIBSTL, *VR41XX_GPSICTL, *VR41XX_KEYEN, *VR41XX_PCS0STRA,
		*VR41XX_PCS0STPA, *VR41XX_PCS0HIA, *VR41XX_PCS1STRA, *VR41XX_PCS1STPA,
		*VR41XX_PCS1HIA, *VR41XX_PCSMODE, *VR41XX_LCDGPMODE, *VR41XX_MISCREG0,
		*VR41XX_MISCREG1, *VR41XX_MISCREG2, *VR41XX_MISCREG3, *VR41XX_MISCREG4,
		*VR41XX_MISCREG5, *VR41XX_MISCREG6, *VR41XX_MISCREG7, *VR41XX_MISCREG8,
		*VR41XX_MISCREG9, *VR41XX_MISCREG10, *VR41XX_MISCREG11, *VR41XX_MISCREG12,
		*VR41XX_MISCREG13, *VR41XX_MISCREG14, *VR41XX_MISCREG15);
#else
	len = sprintf(page,
		"giuiosell:    0x%04hx\n"
		"giuioselh:    0x%04hx\n"
		"giupiodl:     0x%04hx\n"
		"giupiodh:     0x%04hx\n"
		"giupodatl:    0x%04hx\n"
		"\n"
		"giupodath:    0x%04hx\n"
		"giuintstatl:  0x%04hx\n"
		"giuintstath:  0x%04hx\n"
		"giuintenl:    0x%04hx\n"
		"giuintenh:    0x%04hx\n"
		"giuinttypl:   0x%04hx\n"
		"giuinttyph:   0x%04hx\n"
		"giuintalsell: 0x%04hx\n"
		"giuintalselh: 0x%04hx\n"
		"giuinthtsell: 0x%04hx\n"
		"giuinthtselh: 0x%04hx\n"
		"giuusepdn:    0x%04hx\n"
		"giutermupdn:  0x%04hx\n",
		*VR41XX_GIUIOSELL, *VR41XX_GIUIOSELH, *VR41XX_GIUPIODL,
		*VR41XX_GIUPIODH, *VR41XX_GIUPODATL, *VR41XX_GIUPODATH,
		*VR41XX_GIUINTSTATL, *VR41XX_GIUINTSTATH, *VR41XX_GIUINTENL,
		*VR41XX_GIUINTENH, *VR41XX_GIUINTTYPL, *VR41XX_GIUINTTYPH,
		*VR41XX_GIUINTALSELL, *VR41XX_GIUINTALSELH, *VR41XX_GIUINTHTSELL,
		*VR41XX_GIUINTHTSELH, *VR41XX_GIUUSEUPDN, *VR41XX_GIUTERMUPDN);
#endif

	if (len <= off+count) *eof = 1;
	*start = page + off;
	len -= off;
	if (len > count) len = count;
	if (len < 0) len = 0;
	return len;
}

static int __init giuinfo_init(void)
{
	create_proc_read_entry("giuinfo", 0, NULL, get_giuinfo, NULL);
	return 0;
}

__initcall(giuinfo_init);
