/*
 * linux/arch/mips/vr41xx/gpiolcd.c
 *
 * Platform support for the machine which LCD control by GPIO.
 *
 * Copyright (C) 2000 SATO Kazumi
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 */

#include <asm/io.h>
#include <asm/system.h>
#include <asm/vr41xx.h>

#ifdef CONFIG_NEC_MOBILEGEAR2_R300 
#define LCDON   VR41XX_GIUPIODL_GPIO10	/* GPIO[10] 0100 0000 0000 */
#define LCDON_PORT	VR41XX_GIUPIODL
#define LIGHTON VR41XX_GIUPODATL_GPIO45	/* GPIO[45] 10 0000 0000 0000 */
#define LIGHTON_PORT	VR41XX_GIUPODATL
#define MAXCONTRAST	6		/* XXX temporary */
#elif defined(CONFIG_NEC_MOBILEGEAR2_R310)
#define LCDON   VR41XX_GIUPIODL_GPIO10	/* GPIO[10] 0100 0000 0000 */
#define LCDON_PORT	VR41XX_GIUPIODL
#define LIGHTON VR41XX_GIUPODATL_GPIO45	/* GPIO[45] 10 0000 0000 0000 */
#define LIGHTON_PORT	VR41XX_GIUPODATL
#define MAXCONTRAST	6		/* XXX temporary */
#elif defined(CONFIG_CASIO_E15)
#define LCDON   VR41XX_GIUPIODH_GPIO24	/* GPIO[24] 0000 0001 0000 0000 */
#define LCDON_PORT	VR41XX_GIUPIODH
#define LIGHTON VR41XX_GIUPIODH_GPIO26	/* GPIO[26] 0000 0100 0000 0000 */
#define LIGHTON_PORT	VR41XX_GIUPIODH
#define MAXCONTRAST	6		/* XXX temporary */
#else
#error no LCD GPIO Infomation....
#endif

static int backlight, contrast, lcdpower;

void gpiolcd_init_backlight(void)
{
#ifdef STANDALONE
	backlight = 1;
#else /* STANDALONE */
	/* save boot time status (maybe set by Windows CE) */
	if (*LIGHTON_PORT&LIGHTON)
		backlight = 1;
	else
		backlight = 0;
#endif /* STANDALONE */
}

int get_gpiolcd_backlight(void)
{
	return backlight;
}

int gpiolcd_backlight(int n)
{
	int flags;

	if(n == 0) {
		backlight = 0;
		save_and_cli(flags);
		*LIGHTON_PORT &= ~LIGHTON;
		restore_flags(flags);
		// Turn the backlight off.
	} else {
		backlight = 1;
		save_and_cli(flags);
		*LIGHTON_PORT |= LIGHTON;
		restore_flags(flags);
	}
	return 0;
}

void gpiolcd_init_contrast(void)
{
#ifdef STANDALONE
	contrast = MAXCONTRAST;
#else /* STANDALONE */
	/* save boot time status (maybe set by Windows CE) */
	/* but we don't know current value methods, so set constant */
	contrast = MAXCONTRAST;
#endif /* STANDALONE */
}

int get_gpiolcd_contrast(void)
{
	return contrast;
}

int gpiolcd_contrast(int n)
{
	if (n > MAXCONTRAST)
		contrast = MAXCONTRAST;
	else
		contrast = n;
	// Turn the lcd on and some contrast.
	return 0;
}

int get_gpiolcd_lcdpower(void)
{
	return lcdpower;
}

int gpiolcd_lcdpower(int n)
{
	int flags;

	lcdpower = n;
	if(n == 0) {
		save_and_cli(flags);
		*LCDON_PORT &=  ~LCDON;
		restore_flags(flags);
	} else {
		save_and_cli(flags);
		*LCDON_PORT |=  LCDON;
		restore_flags(flags);
	}
	return 0;
}

void gpiolcd_setup(void)
{
	lcdpower = 1; /* boot time always on */
	/* backlight & contrast inherit by WinCE */
	gpiolcd_init_backlight();
	gpiolcd_init_contrast();
	gpiolcd_lcdpower(lcdpower);
	gpiolcd_contrast(contrast);
	gpiolcd_backlight(backlight);
}
