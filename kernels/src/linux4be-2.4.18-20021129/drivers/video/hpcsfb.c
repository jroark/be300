/*
 *  linux/drivers/video/hpcsfb.c
 *
 *  simple framebuffer device w/ HPC device control
 *
 *  For now, this is a crude hack of SFB, which in turn was a crude hack of
 *  virtual frame buffer by Geert Uytterhoeven.  
 *
 *  modified part written by SATO Kazumi.
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License. See the file COPYING in the main directory of this archive for
 *  more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/malloc.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/interrupt.h>
#include <asm/uaccess.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>

#include <video/fbcon.h>
#include <video/fbcon-mfb.h>
#include <video/fbcon-cfb2.h>
#include <video/fbcon-cfb4.h>
#include <video/fbcon-cfb8.h>
#include <video/fbcon-cfb16.h>
#include <video/fbcon-cfb24.h>
#include <video/fbcon-cfb32.h>

#define DEVICE_NAME "hpcsfb"


/*
 * Linux VR uses SFB extensively, so params based on platform in that case
 */
#ifdef CONFIG_CPU_VR41XX
#include <asm/vr41xx-platdep.h>
#endif

/*
 * VIDEORAM_BASE     Address of start of video ram (virtual address)
 * VIDEORAM_SIZE     Size of video ram (default is x * y * (bpp/8)) 
 * FB_X_RES          Visible horizontal resolution in pixels
 * FB_X_VIRTUAL_RES  Horizontal resolution of framebuffer memory in pixels
 *                   (default is FB_X_RES)
 * FB_Y_RES          Visible vertical resolution in pixels
 * FB_Y_VIRTUAL_RES  Vertical resolution of framebuffer memory in pixels
 *                   (default FB_Y_RES)
 * FB_BPP            Bits per pixel
 * FB_PALETTE        The palette for a paletted device, whatever has been
 *                   initialized before boot (since sfb can't change it)
 * FB_IS_GREY        1 means greyscale device, otherwise 0 (default is 0)
 * FB_IS_INVERSE     1 means 0 color value is white instead of black,
 *                   otherwise 0 (default is 0)
 * FB_IS_TRUECOLOR   Define this as 1 to force truecolor for any mode
 *                   less than 16bpp, this is useful for 8bpp truecolor
 *                   devices (this defaults to 1 for 16bpp or greater)
 * The last three are done with 1 / 0 instead of #define / #undef in order
 * to facilitate a generic interface, if we should coose to do one in the
 * future.
 *
 * Some example settings:
 *
 * #define VIDEORAM_BASE	0x12345678
 * #define VIDEORAM_SIZE	(1024 * 1024)
 * #define FB_X_RES		800
 * #define FB_X_VIRTUAL_RES	1024
 * #define FB_Y_RES		600
 * #define FB_Y_VIRTUAL_RES	1024
 * #define FB_BPP		8
 * #define FB_PALETTE		{ {0x12,0x34,0x56,0}, {0x78,0x9a,0xbc,0}, ... }
 * #define FB_IS_GREY		0
 * #define FB_IS_INVERSE	0
 * #define FB_IS_TRUECOLOR	0
 */

/*
 * Please put all hardware-specific defines (or includes) above this line, the
 * frame buffer should be completely described by above parameters.
 */

#ifndef VIDEORAM_BASE
#error "SFB frame buffer parameters have not been defined."
#endif

#ifndef FB_X_VIRTUAL_RES
#define FB_X_VIRTUAL_RES FB_X_RES
#endif

#ifndef FB_Y_VIRTUAL_RES
#define FB_Y_VIRTUAL_RES FB_Y_RES
#endif

#ifndef VIDEORAM_SIZE
#define VIDEORAM_SIZE (FB_X_VIRTUAL_RES * FB_Y_VIRTUAL_RES * FB_BPP / 8)
#endif

#ifndef FB_IS_GREY
#define FB_IS_GREY 0
#endif

#ifndef FB_IS_INVERSE
#define FB_IS_INVERSE 0
#endif

#ifndef FB_IS_TRUECOLOR
#if (FB_BPP >= 16)
#define FB_IS_TRUECOLOR 1
#else
#define FB_IS_TRUECOLOR 0
#endif
#endif

#define arraysize(x)	(sizeof(x)/sizeof(*(x)))

const u_long videomemory = VIDEORAM_BASE, videomemorysize = VIDEORAM_SIZE;

static int currcon = 0;
static struct display disp;
static struct fb_info fb_info;
#if !FB_IS_GREY
#ifdef FB_PALETTE
static struct { u_char red, green, blue, pad; } palette[256] = FB_PALETTE;
#else
static struct { u_char red, green, blue, pad; } palette[256];
#endif
#endif

#if defined(FBCON_HAS_CFB8) && FB_IS_TRUECOLOR || defined(FBCON_HAS_CFB16) || \
    defined(FBCON_HAS_CFB24) || defined(FBCON_HAS_CFB32)
static union {
#if defined(FBCON_HAS_CFB8) && FB_IS_TRUECOLOR
    u8 cfb8[16];
#endif
#ifdef FBCON_HAS_CFB16
    u16 cfb16[16];
#endif
#ifdef FBCON_HAS_CFB24
    u32 cfb24[16];
#endif
#ifdef FBCON_HAS_CFB32
    u32 cfb32[16];
#endif
} fbcon_cmap;
#endif

static char hpcsfb_name[16] = "HPC Simple FB";

static struct fb_var_screeninfo hpcsfb_default = {
    FB_X_RES, FB_Y_RES, FB_X_VIRTUAL_RES, FB_Y_VIRTUAL_RES, 0, 0, FB_BPP, FB_IS_GREY,
    {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
    0, FB_ACTIVATE_NOW, -1, -1, 0, 20000, 64, 64, 32, 32, 64, 2,
    0, FB_VMODE_NONINTERLACED, {0,0,0,0,0,0}
};


    /*
     *  Interface used by the world
     */

int hpcsfb_setup(char*);

static int hpcsfb_open(struct fb_info *info, int user);
static int hpcsfb_release(struct fb_info *info, int user);
static int hpcsfb_get_fix(struct fb_fix_screeninfo *fix, int con,
		       struct fb_info *info);
static int hpcsfb_get_var(struct fb_var_screeninfo *var, int con,
		       struct fb_info *info);
static int hpcsfb_set_var(struct fb_var_screeninfo *var, int con,
		       struct fb_info *info);
static int hpcsfb_pan_display(struct fb_var_screeninfo *var, int con,
			   struct fb_info *info);
static int hpcsfb_get_cmap(struct fb_cmap *cmap, int kspc, int con,
			struct fb_info *info);
static int hpcsfb_set_cmap(struct fb_cmap *cmap, int kspc, int con,
			struct fb_info *info);
static int hpcsfb_ioctl(struct inode *inode, struct file *file, u_int cmd,
		     u_long arg, int con, struct fb_info *info);

#ifdef CONFIG_PROC_FS
static int __init hpcsfb_proc_init(void);
#endif /* CONFIG_PROC_FS */

    /* 
     * Interface to the low level display control
     */
#if defined(CONFIG_NEC_MOBILEGEAR2_R300) || \
    defined(CONFIG_NEC_MOBILEGEAR2_R330) || \
    defined(CONFIG_NEC_MOBILEGEAR2_R430) || \
    defined(CONFIG_NEC_MOBILEGEAR2_R530) || \
    defined(CONFIG_NEC_MOBILEGEAR2_R700) || \
    defined(CONFIG_IBM_WORKPAD) || \
    defined(CONFIG_DOCOMO_SIGMARION)
extern int platdep_setup(void);
extern int display_set__backlight(int);
extern int display_get_backlight(void);
extern int display_power(int);
#define LCD_SETUP()    platdep_setup()
#define LCD_BACKLIGHT(n) display_set_backlight(n)
#define GET_LCD_BACKLIGHT() display_get_backlight()
#define	LCD_CONTRAST(n) display_set_contrast(n)
#define	GET_LCD_CONTRAST() display_get_contrast()
#define LCD_POWER(n) display_power(n)
#define        CAN_SOFT_BLANK  1
#endif

#if !defined(LCD_SETUP)
#error no lcd defined
#define	LCD_SETUP()
#define	LCD_BACKLIGHT(n)
#define	GET_LCD_BACKLIGHT() 0
#define	LCD_CONTRAST(n)
#define	GET_LCD_CONTRAST() 0
#define	LCD_POWER(n)
#endif

#if !defined(CAN_SOFT_BLANK)
#define	CAN_SOFT_BLANK	0
#endif

    /*
     *  Interface to the low level console driver
     */

int hpcsfb_init(void);
static int hpcsfbcon_switch(int con, struct fb_info *info);
static int hpcsfbcon_updatevar(int con, struct fb_info *info);
static void hpcsfbcon_blank(int blank, struct fb_info *info);


    /*
     *  Internal routines
     */

static u_long get_line_length(int xres_virtual, int bpp);
static void hpcsfb_encode_fix(struct fb_fix_screeninfo *fix,
			   struct fb_var_screeninfo *var);
static void set_color_bitfields(struct fb_var_screeninfo *var);
static int hpcsfb_getcolreg(u_int regno, u_int *red, u_int *green, u_int *blue,
                         u_int *transp, struct fb_info *info);
static int hpcsfb_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
                         u_int transp, struct fb_info *info);
static void do_install_cmap(int con, struct fb_info *info);


static struct fb_ops hpcsfb_ops = {
    owner:          THIS_MODULE,
    fb_open:        hpcsfb_open,
    fb_release:     hpcsfb_release,
    fb_get_fix:     hpcsfb_get_fix,
    fb_get_var:     hpcsfb_get_var,
    fb_set_var:     hpcsfb_set_var,
    fb_get_cmap:    hpcsfb_get_cmap,
    fb_set_cmap:    hpcsfb_set_cmap,
    fb_pan_display: hpcsfb_pan_display,
    fb_ioctl:       hpcsfb_ioctl,
};

    /*
     *  Open/Release the frame buffer device
     */

static int hpcsfb_open(struct fb_info *info, int user)
{
    /*                                                                     
     *  Nothing, only a usage count for the moment                          
     */                                                                    

    MOD_INC_USE_COUNT;
    return(0);                              
}
        
static int hpcsfb_release(struct fb_info *info, int user)
{
    MOD_DEC_USE_COUNT;
    return(0);                                                    
}


    /*
     *  Get the Fixed Part of the Display
     */

static int hpcsfb_get_fix(struct fb_fix_screeninfo *fix, int con,
		       struct fb_info *info)
{
    struct fb_var_screeninfo *var;

    if (con == -1)
	var = &hpcsfb_default;
    else
	var = &fb_display[con].var;
    hpcsfb_encode_fix(fix, var);
    return 0;
}


    /*
     *  Get the User Defined Part of the Display
     */

static int hpcsfb_get_var(struct fb_var_screeninfo *var, int con,
		       struct fb_info *info)
{
    if (con == -1)
	*var = hpcsfb_default;
    else
	*var = fb_display[con].var;
    set_color_bitfields(var);
    return 0;
}


    /*
     *  Set the User Defined Part of the Display
     */

static int hpcsfb_set_var(struct fb_var_screeninfo *var, int con,
		       struct fb_info *info)
{
    int err, activate = var->activate;
    int oldxres, oldyres, oldvxres, oldvyres, oldbpp;
    u_long line_length;
    struct display *display;

    if (con >= 0)
	display = &fb_display[con];
    else
	display = &disp;	/* used during initialization */

    /*
     *  FB_VMODE_CONUPDATE and FB_VMODE_SMOOTH_XPAN are equal!
     *  as FB_VMODE_SMOOTH_XPAN is only used internally
     */

    if (var->vmode & FB_VMODE_CONUPDATE) {
	var->vmode |= FB_VMODE_YWRAP;
	var->xoffset = display->var.xoffset;
	var->yoffset = display->var.yoffset;
    }

    /*
     *  Some very basic checks
     */
    if (!var->xres)
	var->xres = 1;
    if (!var->yres)
	var->yres = 1;
    if (var->xres > var->xres_virtual)
	var->xres_virtual = var->xres;
    if (var->yres > var->yres_virtual)
	var->yres_virtual = var->yres;
    if (var->bits_per_pixel <= 1)
	var->bits_per_pixel = 1;
    else if (var->bits_per_pixel <= 2)
	var->bits_per_pixel = 2;
    else if (var->bits_per_pixel <= 4)
	var->bits_per_pixel = 4;
    else if (var->bits_per_pixel <= 8)
	var->bits_per_pixel = 8;
    else if (var->bits_per_pixel <= 16)
	var->bits_per_pixel = 16;
    /* fbcon doesn't support this (yet) */
    else if (var->bits_per_pixel <= 24)
	var->bits_per_pixel = 24;
    else if (var->bits_per_pixel <= 32)
	var->bits_per_pixel = 32;
    else
	return -EINVAL;

    /*
     *  Memory limit
     */
    line_length = get_line_length(var->xres_virtual, var->bits_per_pixel);
    if (line_length*var->yres_virtual > videomemorysize)
	return -ENOMEM;

    set_color_bitfields(var);

    if ((activate & FB_ACTIVATE_MASK) == FB_ACTIVATE_NOW) {
	oldxres = display->var.xres;
	oldyres = display->var.yres;
	oldvxres = display->var.xres_virtual;
	oldvyres = display->var.yres_virtual;
	oldbpp = display->var.bits_per_pixel;
	display->var = *var;
	if (oldxres != var->xres || oldyres != var->yres ||
	    oldvxres != var->xres_virtual || oldvyres != var->yres_virtual ||
	    oldbpp != var->bits_per_pixel) {
	    struct fb_fix_screeninfo fix;

	    hpcsfb_encode_fix(&fix, var);
	    display->screen_base = (char *)videomemory;
	    display->visual = fix.visual;
	    display->type = fix.type;
	    display->type_aux = fix.type_aux;
	    display->ypanstep = fix.ypanstep;
	    display->ywrapstep = fix.ywrapstep;
	    display->line_length = fix.line_length;
	    display->can_soft_blank = CAN_SOFT_BLANK;
	    display->inverse = FB_IS_INVERSE;
	    display->scrollmode = SCROLL_YREDRAW;
	    switch (var->bits_per_pixel) {
#ifdef FBCON_HAS_MFB
		case 1:
		    display->dispsw = &fbcon_mfb;
		    break;
#endif
#ifdef FBCON_HAS_CFB2
		case 2:
		    display->dispsw = &fbcon_cfb2;
		    break;
#endif
#ifdef FBCON_HAS_CFB4
		case 4:
		    display->dispsw = &fbcon_cfb4;
		    break;
#endif
#ifdef FBCON_HAS_CFB8
		case 8:
		    display->dispsw = &fbcon_cfb8;
#if FB_IS_TRUECOLOR
		    display->dispsw_data = fbcon_cmap.cfb8;
#endif
		    break;
#endif
#ifdef FBCON_HAS_CFB16
		case 16:
		    display->dispsw = &fbcon_cfb16;
		    display->dispsw_data = fbcon_cmap.cfb16;
		    break;
#endif
#ifdef FBCON_HAS_CFB24
		case 24:
		    display->dispsw = &fbcon_cfb24;
		    display->dispsw_data = fbcon_cmap.cfb24;
		    break;
#endif
#ifdef FBCON_HAS_CFB32
		case 32:
		    display->dispsw = &fbcon_cfb32;
		    display->dispsw_data = fbcon_cmap.cfb32;
		    break;
#endif
		default:
		    display->dispsw = &fbcon_dummy;
		    break;
	    }
	    if (fb_info.changevar)
		(*fb_info.changevar)(con);
	}
	if (oldbpp != var->bits_per_pixel) {
	    if ((err = fb_alloc_cmap(&display->cmap, 0, 0)))
		return err;
	    do_install_cmap(con, info);
	}
    }
    return 0;
}


    /*
     *  Pan or Wrap the Display
     *
     *  This call looks only at xoffset, yoffset and the FB_VMODE_YWRAP flag
     */

static int hpcsfb_pan_display(struct fb_var_screeninfo *var, int con,
			   struct fb_info *info)
{
#if 1
    return -EINVAL;
#else
    if (var->vmode & FB_VMODE_YWRAP) {
	if (var->yoffset < 0 ||
	    var->yoffset >= fb_display[con].var.yres_virtual ||
	    var->xoffset)
	    return -EINVAL;
    } else {
	if (var->xoffset+fb_display[con].var.xres >
	    fb_display[con].var.xres_virtual ||
	    var->yoffset+fb_display[con].var.yres >
	    fb_display[con].var.yres_virtual)
	    return -EINVAL;
    }
    fb_display[con].var.xoffset = var->xoffset;
    fb_display[con].var.yoffset = var->yoffset;
    if (var->vmode & FB_VMODE_YWRAP)
	fb_display[con].var.vmode |= FB_VMODE_YWRAP;
    else
	fb_display[con].var.vmode &= ~FB_VMODE_YWRAP;
    return 0;
#endif
}

    /*
     *  Get the Colormap
     */

static int hpcsfb_get_cmap(struct fb_cmap *cmap, int kspc, int con,
			struct fb_info *info)
{
    if (con == currcon) /* current console? */
	return fb_get_cmap(cmap, kspc, hpcsfb_getcolreg, info);
    else if (fb_display[con].cmap.len) /* non default colormap? */
	fb_copy_cmap(&fb_display[con].cmap, cmap, kspc ? 0 : 2);
    else
	fb_copy_cmap(fb_default_cmap(1<<fb_display[con].var.bits_per_pixel),
		     cmap, kspc ? 0 : 2);
    return 0;
}

    /*
     *  Set the Colormap
     */

static int hpcsfb_set_cmap(struct fb_cmap *cmap, int kspc, int con,
			struct fb_info *info)
{
    int err;

    if (!fb_display[con].cmap.len) {	/* no colormap allocated? */
	if ((err = fb_alloc_cmap(&fb_display[con].cmap,
			      1<<fb_display[con].var.bits_per_pixel, 0)))
	    return err;
    }
    if (con == currcon)			/* current console? */
	return fb_set_cmap(cmap, kspc, hpcsfb_setcolreg, info);
    else
	fb_copy_cmap(cmap, &fb_display[con].cmap, kspc ? 0 : 1);
    return 0;
}


    /*
     *  Simple Frame Buffer Specific ioctls
     */

static int hpcsfb_ioctl(struct inode *inode, struct file *file, u_int cmd,
		     u_long arg, int con, struct fb_info *info)
{

	int value;

	switch(cmd) {
	case FBIOGET_BACKLIGHT:
		value = GET_LCD_BACKLIGHT();
		put_user(value, (int*)arg);
		return 0;

	case FBIOPUT_BACKLIGHT:
		LCD_BACKLIGHT(arg);
		return 0;

	case FBIOGET_CONTRAST:
		value = GET_LCD_CONTRAST();
		put_user(value, (int*)arg);
		return 0;

	case FBIOPUT_CONTRAST:
		LCD_CONTRAST(arg);
		return 0;

	case FBIO_POWER:
		LCD_POWER(arg);
		return 0;
	}

    return -EINVAL;
}


int __init hpcsfb_setup(char *options)
{
    char *this_opt;

    fb_info.fontname[0] = '\0';

    if (!options || !*options)
	return 0;

    for (this_opt = strtok(options, ","); this_opt;
	 this_opt = strtok(NULL, ",")) {
	if (!strncmp(this_opt, "font:", 5))
	    strcpy(fb_info.fontname, this_opt+5);
    }
    return 0;
}


    /*
     *  Initialisation
     */

int __init hpcsfb_init(void)
{
    strcpy(fb_info.modename, hpcsfb_name);
    fb_info.changevar = NULL;
    fb_info.node = -1;
    fb_info.fbops = &hpcsfb_ops;
    fb_info.disp = &disp;
    fb_info.switch_con = &hpcsfbcon_switch;
    fb_info.updatevar = &hpcsfbcon_updatevar;
    fb_info.blank = &hpcsfbcon_blank;
    fb_info.flags = FBINFO_FLAG_DEFAULT;

    hpcsfb_set_var(&hpcsfb_default, -1, &fb_info);

    if (register_framebuffer(&fb_info) < 0)
	return -EINVAL;

    printk(KERN_INFO "fb%d: HPC Simple frame buffer device, using %ldK of video memory\n",
	   GET_FB_IDX(fb_info.node), videomemorysize>>10);

    LCD_SETUP();

#ifdef CONFIG_PROC_FS
    hpcsfb_proc_init();
#endif /* CONFIG_PROC_FS */

    return 0;
}


static int hpcsfbcon_switch(int con, struct fb_info *info)
{
    /* Do we have to save the colormap? */
    if (fb_display[currcon].cmap.len)
	fb_get_cmap(&fb_display[currcon].cmap, 1, hpcsfb_getcolreg, info);

    currcon = con;
    /* Install new colormap */
    do_install_cmap(con, info);
    return 0;
}

    /*
     *  Update the `var' structure (called by fbcon.c)
     */

static int hpcsfbcon_updatevar(int con, struct fb_info *info)
{
    /* Nothing */
    return 0;
}

    /*
     *  Blank the display.
     */

static void hpcsfbcon_blank(int blank, struct fb_info *info)
{
	static int backlight = -1;

	if (blank || backlight == -1)
		backlight = GET_LCD_BACKLIGHT();

	if (blank) {
		LCD_POWER(0);
		LCD_BACKLIGHT(0);
	} else {
		LCD_POWER(1);
		LCD_BACKLIGHT(backlight);
	}
}

static u_long get_line_length(int xres_virtual, int bpp)
{
    u_long length;
    
    length = (xres_virtual * bpp + 7) >> 3;
    return(length);
}

static void hpcsfb_encode_fix(struct fb_fix_screeninfo *fix,
			   struct fb_var_screeninfo *var)
{
    memset(fix, 0, sizeof(struct fb_fix_screeninfo));
    strcpy(fix->id, hpcsfb_name);
    fix->smem_start = videomemory;
    fix->smem_len = videomemorysize;
    fix->type = FB_TYPE_PACKED_PIXELS;
    fix->type_aux = 0;
    switch (var->bits_per_pixel) {
	case 1:
	    fix->visual = FB_VISUAL_MONO01;
	    break;
	case 2:
	case 4:
	case 8:
	    fix->visual = FB_VISUAL_STATIC_PSEUDOCOLOR;
	    break;
	case 16:
	case 24:
	case 32:
	    fix->visual = FB_VISUAL_TRUECOLOR;
	    break;
    }
    fix->ywrapstep = 1;
    fix->xpanstep = 1;
    fix->ypanstep = 1;
    fix->line_length = get_line_length(var->xres_virtual, var->bits_per_pixel);
}

static void set_color_bitfields(struct fb_var_screeninfo *var)
{
    switch (var->bits_per_pixel) {
	case 1:
	case 2:
	case 4:
	case 8:
#if FB_IS_TRUECOLOR
	    var->red.offset = 5;
	    var->red.length = 3;
	    var->green.offset = 2;
	    var->green.length = 3;
	    var->blue.offset = 0;
	    var->blue.length = 2;
#else
	    var->red.offset = 0;
	    var->red.length = var->bits_per_pixel;
	    var->green.offset = 0;
	    var->green.length = var->bits_per_pixel;
	    var->blue.offset = 0;
	    var->blue.length = var->bits_per_pixel;
#endif
	    var->transp.offset = 0;
	    var->transp.length = 0;
	    break;
	case 16:  // RGB 565
	    var->red.offset = 11;
	    var->red.length = 5;
	    var->green.offset = 5;
	    var->green.length = 6;
	    var->blue.offset = 0;
	    var->blue.length = 5;
	    var->transp.offset = 0;
	    var->transp.length = 0;
	    break;

	case 24: // RGB 888
	    var->red.offset = 16;
	    var->red.length = 8;
	    var->green.offset = 8;
	    var->green.length = 8;
	    var->blue.offset = 0;
	    var->blue.length = 8;
	    var->transp.offset = 0;
	    var->transp.length = 0;
	    break;
	case 32:	/* RGBA 8888 */
	    var->red.offset = 16;
	    var->red.length = 8;
	    var->green.offset = 8;
	    var->green.length = 8;
	    var->blue.offset = 0;
	    var->blue.length = 8;
	    var->transp.offset = 24;
	    var->transp.length = 8;
	    break;
    }
    var->red.msb_right = 0;
    var->green.msb_right = 0;
    var->blue.msb_right = 0;
    var->transp.msb_right = 0;
}


#if FB_IS_TRUECOLOR
#define MAX_REGNO 15
#else
#define MAX_REGNO ((1 << FB_BPP) - 1)
#endif

    /*
     *  Read a single color register and split it into
     *  colors/transparent. Return != 0 for invalid regno.
     *
     *  For palette or greyscale mode, returns what the hardware will
     *  actually do for that index (as far as we know), instead of the
     *  value set by setcolreg, since the hardware palette cannot be
     *  changed by this driver.  For palette mode, we rely on whatever
     *  is set by FB_PALETTE, if not defined, then we just return 0s...
     */

static int hpcsfb_getcolreg(u_int regno, u_int *red, u_int *green, u_int *blue,
                         u_int *transp, struct fb_info *info)
{
    if (regno > MAX_REGNO)
	return 1;

#if FB_IS_GREY
    {
	u_int grey;

	grey = regno * 255 / MAX_REGNO;
#if FB_IS_INVERSE
	grey ^= 255;
#endif
	grey |= grey << 8;
	*red = grey;
	*green = grey;
	*blue = grey;
    }
#else /* either truecolor or palatted */
    *red = (palette[regno].red << 8) | palette[regno].red;
    *green = (palette[regno].green << 8) | palette[regno].green;
    *blue = (palette[regno].blue << 8) | palette[regno].blue;
#endif
    *transp = 0;
    return 0;
}


    /*
     *  Set a single color register. The values supplied are already
     *  rounded down to the hardware's capabilities (according to the
     *  entries in the var structure). Return != 0 for invalid regno.
     *
     *  For paletted and greyscale modes, this just ignores the value
     *  being set in the color register.
     */

static int hpcsfb_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
                         u_int transp, struct fb_info *info)
{
    if (regno > MAX_REGNO)
	return 1;

#if FB_IS_TRUECOLOR
    red >>= 8;
    green >>= 8;
    blue >>= 8;

    /*
     *  Round color down to what the video mode can actually do, so if the
     *  palette is read back, the actual hardware value is returned.  Also, 
     *  maintain index to RGB map for fbcon (accessed through dispsw_data)
     */
#if (FB_BPP == 8)
    red   &= 0xe0;
    green &= 0xe0;
    blue  &= 0xc0;
#if defined(FBCON_HAS_CFB8)
    fbcon_cmap.cfb8[regno] = red | (green >> 3) | (blue >> 6);
#endif 
#endif 

#if (FB_BPP == 16)
    red   &= 0xF8;
    green &= 0xFC;
    blue  &= 0xF8;
#if defined(FBCON_HAS_CFB16)
    fbcon_cmap.cfb16[regno] = (red << 8) | (green << 3) | (blue >> 3);
#endif 
#endif 

#if (FB_BPP == 24) && defined(FBCON_HAS_CFB24)
    fbcon_cmap.cfb24[regno] = (red << 16) | (green << 8) | blue;
#endif

#if (FB_BPP == 32) && defined(FBCON_HAS_CFB32)
    fbcon_cmap.cfb32[regno] = (red << 16) | (green << 8) | blue;
#endif

    palette[regno].red = red;
    palette[regno].green = green;
    palette[regno].blue = blue;
#endif /* FB_IS_TRUECOLOR */

    return 0;
}


static void do_install_cmap(int con, struct fb_info *info)
{
    if (con != currcon)
	return;
    if (fb_display[con].cmap.len)
	fb_set_cmap(&fb_display[con].cmap, 1, hpcsfb_setcolreg, info);
    else
	fb_set_cmap(fb_default_cmap(1<<fb_display[con].var.bits_per_pixel), 1,
		    hpcsfb_setcolreg, info);
}

#ifdef MODULE
int init_module(void)
{
    return hpcsfb_init();
}

void cleanup_module(void)
{
    unregister_framebuffer(&fb_info);
}

#endif /* MODULE */

#ifdef CONFIG_PM
static int hpcsfb_pm_request(struct pm_dev *dev, pm_request_t rqst, void *data)
{
	static int backlight = 0;

	switch (rqst) {
	case PM_SUSPEND:
		printk(KERN_DEBUG DEVICE_NAME ": powering off\n");
		backlight = GET_LCD_BACKLIGHT(); /* save backlight state */
		LCD_BACKLIGHT(0);
		LCD_POWER(0);
		break;

	case PM_RESUME:
		printk(KERN_DEBUG DEVICE_NAME ": powering on\n");
		LCD_POWER(1);
		LCD_BACKLIGHT(backlight); /* resume backlight state */
		break;
	}
	return 0;
}

static int __init hpcsfb_pm_init(void)
{
	printk(KERN_DEBUG DEVICE_NAME ": pm_init\n");
	pm_register(PM_SYS_DEV, PM_SYS_UNKNOWN, hpcsfb_pm_request);
	return 0;
}

__initcall(hpcsfb_pm_init);
#endif	// CONFIG_PM

#ifdef CONFIG_PROC_FS
static ssize_t hpcsfb_backlight_read(struct file *file, char *buf, size_t len,
					loff_t *ppos)
{
	static char tmpbuf[12];	/* XXX */
	size_t rlen;

	if (*ppos != 0)
		// return -EIO;
		return 0;

	sprintf(tmpbuf, "%d\n", GET_LCD_BACKLIGHT());
	rlen = strlen(tmpbuf);

	if (len < rlen)
		return -EIO;
	if (copy_to_user(buf, tmpbuf, rlen))
		return -EFAULT;
	*ppos += rlen;
	return rlen;
}

static ssize_t hpcsfb_contrast_read(struct file *file, char *buf, size_t len,
					loff_t *ppos)
{
	static char tmpbuf[12];	/* XXX */
	size_t rlen;

	if (*ppos != 0)
		// return -EIO;
		return 0;

	sprintf(tmpbuf, "%d\n", GET_LCD_CONTRAST());
	rlen = strlen(tmpbuf);

	if (len < rlen)
		return -EIO;
	if (copy_to_user(buf, tmpbuf, rlen))
		return -EFAULT;
	*ppos += rlen;
	return rlen;
}

static int atoi(const char *p)
{
	int n = 0;
	while (*p && '0' <= *p && *p <= '9') {
		n *= 10;
		n += *p - '0';
		p++;
	}
	return (n);
}

static ssize_t hpcsfb_backlight_write(struct file *file, char *buf, size_t len,
					loff_t *ppos)
{
	static char tmpbuf[12];	/* XXX */
	int backlight;

	if (*ppos != 0)
		return -EIO;

	if (len > 11)
		return -EIO;

	if (copy_from_user(tmpbuf, buf, len))
		return -EFAULT;
	tmpbuf[len] = 0;
	
	if (tmpbuf[0] >= '0' && tmpbuf[9] <= '9')	
		backlight = atoi(tmpbuf);
	else
		return -EIO;
	LCD_BACKLIGHT(backlight);
	*ppos += len;
	return len;
}

static ssize_t hpcsfb_contrast_write(struct file *file, char *buf, size_t len,
					loff_t *ppos)
{
	static char tmpbuf[12];	/* XXX */
	int contrast;

	if (*ppos != 0)
		return -EIO;

	if (len > 11)
		return -EIO;

	if (copy_from_user(tmpbuf, buf, len))
		return -EFAULT;
	tmpbuf[len] = 0;
	
	if (tmpbuf[0] >= '0' && tmpbuf[9] <= '9')	
		contrast = atoi(tmpbuf);
	else
		return -EIO;
	LCD_CONTRAST(contrast);
	*ppos += len;
	return len;
}

static struct file_operations backlight_fops = 
{
	read:	hpcsfb_backlight_read,
	write:	hpcsfb_backlight_write,
};

static struct file_operations contrast_fops = 
{
	read:	hpcsfb_contrast_read,
	write:	hpcsfb_contrast_write,
};

static struct proc_dir_entry *proc_root_backlight;
static struct proc_dir_entry *proc_root_contrast;

static int __init hpcsfb_proc_init(void)
{
	proc_root_backlight = create_proc_entry("backlight", 
					S_IWUSR | S_IRUGO, &proc_root);
	proc_root_backlight->proc_fops = &backlight_fops;

	proc_root_contrast = create_proc_entry("contrast", 
					S_IWUSR | S_IRUGO, &proc_root);
	proc_root_contrast->proc_fops = &contrast_fops;
	
	return 0;
}
#endif /* CONFIG_PROC_FS */
