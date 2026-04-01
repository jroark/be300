/*
	linux/drivers/sound/vr41xx.c

	VR41XX DMA based 10bit DAC sound driver derived from dmasound.c (of AMIGA)

	Copyright (C) 2000,2001 by Hiroshi Kawashima <kawashima@iname.com>

	This is distributed under the GNU GENERAL PUBLIC LICENSE (GPL)
	Version 2 (June 1991). See the "COPYING" file distributed with this
	software for more info.
*/

/*
	**** Supported feature ****
	* Linear signed/unsigned 8/16bit big/little endian PCM.
	* 8bit A-Law/Mu-Law.
	* Stereo -> Mono conversion (just compute average of R/L-ch).

	**** Known Limitation ****
	* Will work only on :
		* MobileGear-II/Pro
		* IBM WorkPad z50
		* Everex Freestyle
		* Compaq Aero 15xx
	  platforms. (Because speaker On/Off code (via GPIO) depends on
	  each platform).
	  Tested on MobileGear-II/R300/R450 (Japanese model) && z50.
	* Volume setting is ignored. (always full volume)
	
	Enjoy! -- kawashima
*/

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/major.h>
#include <linux/config.h>
#include <linux/fcntl.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/sound.h>
#include <linux/init.h>

#ifdef CONFIG_SOUND_VR41XX
#  include <linux/delay.h>
#endif

#include <asm/system.h>
#include <asm/irq.h>
#include <asm/pgtable.h>
#include <asm/uaccess.h>

#include <asm/vr41xx.h>
#include <asm/vr41xx-platdep.h>

#include <linux/soundcard.h>

// #define DEBUG	1

#ifdef CONFIG_SOUND_VR41XX
#  define	HAS_10BIT_TABLES
#else
#  define	HAS_8BIT_TABLES
#endif /* CONFIG_SOUND_VR41XX */

#ifdef MODULE
static int sq_unit = -1;
static int mixer_unit = -1;
static int state_unit = -1;
static int irq_installed = 0;
#endif /* MODULE */
static char **sound_buffers = NULL;
#ifdef CONFIG_SOUND_VR41XX
static char **vr41xx_dma_bufs = NULL;
#endif

/*** Some declarations *******************************************************/
/*==== Copied from old dmasound.h ====*/
/*
 * Minor numbers for the sound driver.
 *
 * Unfortunately Creative called the codec chip of SB as a DSP. For this
 * reason the /dev/dsp is reserved for digitized audio use. There is a
 * device for true DSP processors but it will be called something else.
 * In v3.0 it's /dev/sndproc but this could be a temporary solution.
 */

#define SND_NDEVS	256		/* Number of supported devices */
#define SND_DEV_CTL	0		/* Control port /dev/mixer */
#define SND_DEV_SEQ	1		/* Sequencer output /dev/sequencer (FM
							   synthesizer and MIDI output) */
#define SND_DEV_MIDIN	2	/* Raw midi access */
#define SND_DEV_DSP		3	/* Digitized voice /dev/dsp */
#define SND_DEV_AUDIO	4	/* Sparc compatible /dev/audio */
#define SND_DEV_DSP16	5	/* Like /dev/dsp but 16 bits/sample */
#define SND_DEV_STATUS	6	/* /dev/sndstat */
/* #7 not in use now. Was in 2.4. Free for use after v3.0. */
#define SND_DEV_SEQ2	8	/* /dev/sequencer, level 2 interface */
#define SND_DEV_SNDPROC 9	/* /dev/sndproc for programmable devices */
#define SND_DEV_PSS	SND_DEV_SNDPROC

#define DSP_DEFAULT_SPEED	8000

#define ON		1
#define OFF		0

#define MAX_AUDIO_DEV	5
#define MAX_MIXER_DEV	2
#define MAX_SYNTH_DEV	3
#define MAX_MIDI_DEV	6
#define MAX_TIMER_DEV	3
/*====================================*/

#define DMASND_TT				1
#define DMASND_FALCON			2
#define DMASND_AMIGA			3
#define DMASND_AWACS			4
#ifdef CONFIG_SOUND_VR41XX
#  define	DMASND_VR41XX		5
#endif

#define MAX_CATCH_RADIUS		10
#define MIN_BUFFERS				4
#ifdef CONFIG_SOUND_VR41XX
#  define MIN_BUFSIZE 			2	/* Always just only 2K byte allowed */
#  define MAX_BUFSIZE			2	/* Always just only 2K byte allowed */
#else
#  define MIN_BUFSIZE 			4
#  define MAX_BUFSIZE			128	/* Limit for Amiga */
#endif

static int catchRadius = 0;
/* Real buffer size is bufSize * 1024 byte */
#ifdef CONFIG_SOUND_VR41XX
static int numBufs = 64, bufSize = 2;
#else
static int numBufs = 4, bufSize = 32;
#endif

MODULE_PARM(catchRadius, "i");
MODULE_PARM(numBufs, "i");
MODULE_PARM(bufSize, "i");
MODULE_PARM(numreadBufs, "i");
MODULE_PARM(readbufSize, "i");

#define arraysize(x)	(sizeof(x)/sizeof(*(x)))
#define min(x, y)	((x) < (y) ? (x) : (y))
#define le2be16(x)	(((x)<<8 & 0xff00) | ((x)>>8 & 0x00ff))
#define le2be16dbl(x)	(((x)<<8 & 0xff00ff00) | ((x)>>8 & 0x00ff00ff))

#define IOCTL_IN(arg, ret) \
	do { int error = get_user(ret, (int *)(arg)); \
		if (error) return error; \
	} while (0)
#define IOCTL_OUT(arg, ret)	ioctl_return((int *)(arg), ret)


/*** Some low level helpers **************************************************/

#ifdef HAS_8BIT_TABLES
/* 8 bit mu-law */

static char ulaw2dma8[] = {
	-126,	-122,	-118,	-114,	-110,	-106,	-102,	-98,
	-94,	-90,	-86,	-82,	-78,	-74,	-70,	-66,
	-63,	-61,	-59,	-57,	-55,	-53,	-51,	-49,
	-47,	-45,	-43,	-41,	-39,	-37,	-35,	-33,
	-31,	-30,	-29,	-28,	-27,	-26,	-25,	-24,
	-23,	-22,	-21,	-20,	-19,	-18,	-17,	-16,
	-16,	-15,	-15,	-14,	-14,	-13,	-13,	-12,
	-12,	-11,	-11,	-10,	-10,	-9,	-9,	-8,
	-8,	-8,	-7,	-7,	-7,	-7,	-6,	-6,
	-6,	-6,	-5,	-5,	-5,	-5,	-4,	-4,
	-4,	-4,	-4,	-4,	-3,	-3,	-3,	-3,
	-3,	-3,	-3,	-3,	-2,	-2,	-2,	-2,
	-2,	-2,	-2,	-2,	-2,	-2,	-2,	-2,
	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
	-1,	-1,	-1,	-1,	-1,	-1,	-1,	0,
	125,	121,	117,	113,	109,	105,	101,	97,
	93,	89,	85,	81,	77,	73,	69,	65,
	62,	60,	58,	56,	54,	52,	50,	48,
	46,	44,	42,	40,	38,	36,	34,	32,
	30,	29,	28,	27,	26,	25,	24,	23,
	22,	21,	20,	19,	18,	17,	16,	15,
	15,	14,	14,	13,	13,	12,	12,	11,
	11,	10,	10,	9,	9,	8,	8,	7,
	7,	7,	6,	6,	6,	6,	5,	5,
	5,	5,	4,	4,	4,	4,	3,	3,
	3,	3,	3,	3,	2,	2,	2,	2,
	2,	2,	2,	2,	1,	1,	1,	1,
	1,	1,	1,	1,	1,	1,	1,	1,
	0,	0,	0,	0,	0,	0,	0,	0,
	0,	0,	0,	0,	0,	0,	0,	0,
	0,	0,	0,	0,	0,	0,	0,	0
};

/* 8 bit A-law */

static char alaw2dma8[] = {
	-22,	-21,	-24,	-23,	-18,	-17,	-20,	-19,
	-30,	-29,	-32,	-31,	-26,	-25,	-28,	-27,
	-11,	-11,	-12,	-12,	-9,	-9,	-10,	-10,
	-15,	-15,	-16,	-16,	-13,	-13,	-14,	-14,
	-86,	-82,	-94,	-90,	-70,	-66,	-78,	-74,
	-118,	-114,	-126,	-122,	-102,	-98,	-110,	-106,
	-43,	-41,	-47,	-45,	-35,	-33,	-39,	-37,
	-59,	-57,	-63,	-61,	-51,	-49,	-55,	-53,
	-2,	-2,	-2,	-2,	-2,	-2,	-2,	-2,
	-2,	-2,	-2,	-2,	-2,	-2,	-2,	-2,
	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
	-6,	-6,	-6,	-6,	-5,	-5,	-5,	-5,
	-8,	-8,	-8,	-8,	-7,	-7,	-7,	-7,
	-3,	-3,	-3,	-3,	-3,	-3,	-3,	-3,
	-4,	-4,	-4,	-4,	-4,	-4,	-4,	-4,
	21,	20,	23,	22,	17,	16,	19,	18,
	29,	28,	31,	30,	25,	24,	27,	26,
	10,	10,	11,	11,	8,	8,	9,	9,
	14,	14,	15,	15,	12,	12,	13,	13,
	86,	82,	94,	90,	70,	66,	78,	74,
	118,	114,	126,	122,	102,	98,	110,	106,
	43,	41,	47,	45,	35,	33,	39,	37,
	59,	57,	63,	61,	51,	49,	55,	53,
	1,	1,	1,	1,	1,	1,	1,	1,
	1,	1,	1,	1,	1,	1,	1,	1,
	0,	0,	0,	0,	0,	0,	0,	0,
	0,	0,	0,	0,	0,	0,	0,	0,
	5,	5,	5,	5,	4,	4,	4,	4,
	7,	7,	7,	7,	6,	6,	6,	6,
	2,	2,	2,	2,	2,	2,	2,	2,
	3,	3,	3,	3,	3,	3,	3,	3
};
#endif /* HAS_8BIT_TABLES */

#ifdef HAS_10BIT_TABLES
/* 10-bit mu-law */
static unsigned short ulaw2dma10[] = {
      10,   26,   42,   58,   74,   90,  106,  122,
     138,  154,  170,  186,  202,  218,  234,  250,
     262,  270,  278,  286,  294,  302,  310,  318,
     326,  334,  342,  350,  358,  366,  374,  382,
     388,  392,  396,  400,  404,  408,  412,  416,
     420,  424,  428,  432,  436,  440,  444,  448,
     451,  453,  455,  457,  459,  461,  463,  465,
     467,  469,  471,  473,  475,  477,  479,  481,
     482,  483,  484,  485,  486,  487,  488,  489,
     490,  491,  492,  493,  494,  495,  496,  497,
     498,  498,  499,  499,  500,  500,  501,  501,
     502,  502,  503,  503,  504,  504,  505,  505,
     506,  506,  506,  506,  507,  507,  507,  507,
     508,  508,  508,  508,  509,  509,  509,  509,
     510,  510,  510,  510,  510,  510,  510,  510,
     511,  511,  511,  511,  511,  511,  511,  511,
    1013,  997,  981,  965,  949,  933,  917,  901,
     885,  869,  853,  837,  821,  805,  789,  773,
     761,  753,  745,  737,  729,  721,  713,  705,
     697,  689,  681,  673,  665,  657,  649,  641,
     635,  631,  627,  623,  619,  615,  611,  607,
     603,  599,  595,  591,  587,  583,  579,  575,
     572,  570,  568,  566,  564,  562,  560,  558,
     556,  554,  552,  550,  548,  546,  544,  542,
     541,  540,  539,  538,  537,  536,  535,  534,
     533,  532,  531,  530,  529,  528,  527,  526,
     525,  525,  524,  524,  523,  523,  522,  522,
     521,  521,  520,  520,  519,  519,  518,  518,
     517,  517,  517,  517,  516,  516,  516,  516,
     515,  515,  515,  515,  514,  514,  514,  514,
     513,  513,  513,  513,  513,  513,  513,  512,
     512,  512,  512,  512,  512,  512,  512,  511
};

/* 10-bit A-law */
static unsigned short alaw2dma10[] = {
     425,  429,  417,  421,  441,  445,  433,  437,
     393,  397,  385,  389,  409,  413,  401,  405,
     468,  470,  464,  466,  476,  478,  472,  474,
     452,  454,  448,  450,  460,  462,  456,  458,
     167,  183,  135,  151,  231,  247,  199,  215,
      39,   55,    7,   23,  103,  119,   71,   87,
     339,  347,  323,  331,  371,  379,  355,  363,
     275,  283,  259,  267,  307,  315,  291,  299,
     506,  506,  506,  506,  507,  507,  507,  507,
     504,  504,  504,  504,  505,  505,  505,  505,
     510,  510,  510,  510,  511,  511,  511,  511,
     508,  508,  508,  508,  509,  509,  509,  509,
     490,  491,  488,  489,  494,  495,  492,  493,
     482,  483,  480,  481,  486,  487,  484,  485,
     501,  501,  500,  500,  503,  503,  502,  502,
     497,  497,  496,  496,  499,  499,  498,  498,
     597,  593,  605,  601,  581,  577,  589,  585,
     629,  625,  637,  633,  613,  609,  621,  617,
     554,  552,  558,  556,  546,  544,  550,  548,
     570,  568,  574,  572,  562,  560,  566,  564,
     855,  839,  887,  871,  791,  775,  823,  807,
     983,  967, 1015,  999,  919,  903,  951,  935,
     683,  675,  699,  691,  651,  643,  667,  659,
     747,  739,  763,  755,  715,  707,  731,  723,
     517,  517,  517,  517,  516,  516,  516,  516,
     519,  519,  519,  519,  518,  518,  518,  518,
     513,  513,  513,  513,  512,  512,  512,  512,
     515,  515,  515,  515,  514,  514,  514,  514,
     533,  532,  535,  534,  529,  528,  531,  530,
     541,  540,  543,  542,  537,  536,  539,  538,
     522,  522,  523,  523,  520,  520,  521,  521,
     526,  526,  527,  527,  524,  524,  525,  525
};
#endif	/* HAS_10BIT_TABLES */

/*** Translations ************************************************************/
#ifdef CONFIG_SOUND_VR41XX
static ssize_t vr41xx_ct_law(const u_char *userPtr, size_t userCount,
			  u_char frame[], ssize_t *frameUsed,
			  ssize_t frameLeft);
static ssize_t vr41xx_ct_s8(const u_char *userPtr, size_t userCount,
			 u_char frame[], ssize_t *frameUsed,
			 ssize_t frameLeft);
static ssize_t vr41xx_ct_u8(const u_char *userPtr, size_t userCount,
			 u_char frame[], ssize_t *frameUsed,
			 ssize_t frameLeft);
#endif /* CONFIG_SOUND_VR41XX */

/*** Machine definitions *****************************************************/


typedef struct {
	int type;
	void *(*dma_alloc)(unsigned int, int);
	void (*dma_free)(void *, unsigned int);
	int (*irqinit)(void);
#ifdef MODULE
	void (*irqcleanup)(void);
#endif /* MODULE */
	void (*init)(void);
	void (*silence)(void);
	int (*setFormat)(int);
	int (*setVolume)(int);
	int (*setBass)(int);
	int (*setTreble)(int);
	int (*setGain)(int);
	void (*play)(void);
} MACHINE;


/*** Low level stuff *********************************************************/


typedef struct {
	int format;		/* AFMT_* */
	int stereo;		/* 0 = mono, 1 = stereo */
	int size;		/* 8/16 bit*/
	int speed;		/* speed */
} SETTINGS;

typedef struct {
	ssize_t (*ct_ulaw)(const u_char *, size_t, u_char *, ssize_t *, ssize_t);
	ssize_t (*ct_alaw)(const u_char *, size_t, u_char *, ssize_t *, ssize_t);
	ssize_t (*ct_s8)(const u_char *, size_t, u_char *, ssize_t *, ssize_t);
	ssize_t (*ct_u8)(const u_char *, size_t, u_char *, ssize_t *, ssize_t);
	ssize_t (*ct_s16be)(const u_char *, size_t, u_char *, ssize_t *, ssize_t);
	ssize_t (*ct_u16be)(const u_char *, size_t, u_char *, ssize_t *, ssize_t);
	ssize_t (*ct_s16le)(const u_char *, size_t, u_char *, ssize_t *, ssize_t);
	ssize_t (*ct_u16le)(const u_char *, size_t, u_char *, ssize_t *, ssize_t);
} TRANS;

struct sound_settings {
	MACHINE mach;		/* machine dependent things */
	SETTINGS hard;		/* hardware settings */
	SETTINGS soft;		/* software settings */
	SETTINGS dsp;		/* /dev/dsp default settings */
	TRANS *trans;		/* supported translations */
	int volume_left;	/* volume (range is machine dependent) */
	int volume_right;
	int bass;		/* tone (range is machine dependent) */
	int treble;
	int gain;
	int minDev;		/* minor device number currently open */
};

static struct sound_settings sound;

#ifdef CONFIG_SOUND_VR41XX
static int Vr41xxIrqInit(void);
#ifdef MODULE
static void Vr41xxIrqCleanUp(void);
#endif /* MODULE */
static void Vr41xxSilence(void);
static void Vr41xxInit(void);
static int Vr41xxSetFormat(int format);
/* static int Vr41xxSetVolume(int volume); */
/* static int Vr41xxSetTreble(int treble); */
static void vr41xx_sq_play_next_frame(int index);
static void Vr41xxPlay(void);
static void vr41xx_sq_interrupt(int irq, void *dummy, struct pt_regs *fp);
#endif /* CONFIG_SOUND_VR41XX */


/*** Mid level stuff *********************************************************/


static void sound_silence(void);
static void sound_init(void);
static int sound_set_format(int format);
static int sound_set_speed(int speed);
static int sound_set_stereo(int stereo);
// static int sound_set_volume(int volume);
static ssize_t sound_copy_translate(const u_char *userPtr,
				    size_t userCount,
				    u_char frame[], ssize_t *frameUsed,
				    ssize_t frameLeft);
/*
 * /dev/mixer abstraction
 */

struct sound_mixer {
    int busy;
    int modify_counter;
};

static struct sound_mixer mixer;

/*
 * Sound queue stuff, the heart of the driver
 */

struct sound_queue {
	int max_count, block_size;
	char **buffers;
	int max_active;

	/* it shouldn't be necessary to declare any of these volatile */
	int front, rear, count;
	int rear_size;
	/*
	 *	The use of the playing field depends on the hardware
	 *
	 *	Atari, PMac: The number of frames that are loaded/playing
	 *
	 *	Amiga: Bit 0 is set: a frame is loaded
	 *	       Bit 1 is set: a frame is playing
	 */
	int active;
	wait_queue_head_t action_queue, open_queue, sync_queue;
	int open_mode;
	int busy, syncing;
#ifdef CONFIG_SOUND_VR41XX
	u_short wake_up_threshold;
#endif
};

static struct sound_queue sq;

#define sq_block_address(i)	(sq.buffers[i])
#define SIGNAL_RECEIVED	(signal_pending(current))
#define NON_BLOCKING(open_mode)	(open_mode & O_NONBLOCK)
#define ONE_SECOND	HZ	/* in jiffies (100ths of a second) */
#define NO_TIME_LIMIT	0xffffffff
#define SLEEP(queue, time_limit) \
	interruptible_sleep_on_timeout(&queue, (time_limit));
#define WAKE_UP(queue)	(wake_up_interruptible(&queue))

/*
 * /dev/sndstat
 */

struct sound_state {
	int busy;
	char buf[512];
	int len, ptr;
};

static struct sound_state state;

/*** Common stuff ********************************************************/

static loff_t sound_lseek(struct file *file, long long offset, int orig);
static inline int ioctl_return(int *addr, int value)
{
	if (value < 0)
		return(value);

	return put_user(value, addr);
}


/*** Translations ************************************************************/


/* ++TeSche: radically changed for new expanding purposes...
 *
 * These two routines now deal with copying/expanding/translating the samples
 * from user space into our buffer at the right frequency. They take care about
 * how much data there's actually to read, how much buffer space there is and
 * to convert samples into the right frequency/encoding. They will only work on
 * complete samples so it may happen they leave some bytes in the input stream
 * if the user didn't write a multiple of the current sample size. They both
 * return the number of bytes they've used from both streams so you may detect
 * such a situation. Luckily all programs should be able to cope with that.
 *
 * I think I've optimized anything as far as one can do in plain C, all
 * variables should fit in registers and the loops are really short. There's
 * one loop for every possible situation. Writing a more generalized and thus
 * parameterized loop would only produce slower code. Feel free to optimize
 * this in assembler if you like. :)
 *
 * I think these routines belong here because they're not yet really hardware
 * independent, especially the fact that the Falcon can play 16bit samples
 * only in stereo is hardcoded in both of them!
 *
 * ++geert: split in even more functions (one per format)
 */

#ifdef CONFIG_SOUND_VR41XX

static inline void vr41xx_fill(u_short d, u_short *p, int n)
{
	while (n--)
		*p++ = d;
}

static ssize_t vr41xx_ct_law(const u_char *userPtr, size_t userCount,
			  u_char frame[], ssize_t *frameUsed,
			  ssize_t frameLeft)
{
	u_short *table =
		(sound.soft.format == AFMT_MU_LAW) ? ulaw2dma10 : alaw2dma10;
	ssize_t count, uUsed, fUsed;
	u_short *p = (u_short *)(&frame[*frameUsed]);

	if (sound.hard.stereo) {
		count = min(userCount >> 1, frameLeft >> 1);
		uUsed = count * 2;
		fUsed = count * 2;
	} else {
		count = min(userCount, frameLeft >> 1);
		uUsed = count;
		fUsed = count * 2;
	}
	while (count > 0) {
		u_char data;
		u_short d;
		if (get_user(data, userPtr++))
			return -EFAULT;
		d = data;
		if (sound.hard.stereo) {
			// Translate stereo -> mono
			if (get_user(data, userPtr++))
				return -EFAULT;
			d += data;
			d >>= 1;	// take average of R/L ch
		}
		*p++ = ((u_short)table[d]);
		count--;
	}
	*frameUsed += fUsed;
	// Clear rest of frame
	vr41xx_fill(0x0200, p, (frameLeft - fUsed) >> 1);

	return(uUsed);
}


static ssize_t vr41xx_ct_s8(const u_char *userPtr, size_t userCount,
			 u_char frame[], ssize_t *frameUsed, ssize_t frameLeft)
{
	ssize_t count, uUsed, fUsed;

	u_short *p = (u_short *)(&frame[*frameUsed]);
	if (sound.hard.stereo) {
		count = min(userCount >> 1, frameLeft >> 1);
		uUsed = count * 2;
		fUsed = count * 2;
	} else {
		count = min(userCount, frameLeft >> 1);
		uUsed = count;
		fUsed = count * 2;
	}
	while (count > 0) {
		u_char data;
		u_short d;
		if (get_user(data, userPtr++))
			return -EFAULT;
		d = data;
		if (sound.hard.stereo) {
			// Translate stereo -> mono
			if (get_user(data, userPtr++))
				return -EFAULT;
			d += data;
			d >>= 1;	// take average of R/L ch
		}
		*p++ = (d << 2) ^ 0x0200;
		count--;
	}
	*frameUsed += fUsed;
	// Clear rest of frame
	vr41xx_fill(0x0200, p, (frameLeft - fUsed) >> 1);

	return (uUsed);
}


static ssize_t vr41xx_ct_u8(const u_char *userPtr, size_t userCount,
			 u_char frame[], ssize_t *frameUsed, ssize_t frameLeft)
{
	ssize_t count, uUsed, fUsed;

	u_short  *p = (u_short *)(&frame[*frameUsed]);
	if (sound.hard.stereo) {
		count = min(userCount >> 1, frameLeft >> 1);
		uUsed = count * 2;
		fUsed = count * 2;
	} else {
		count = min(userCount, frameLeft >> 1);
		uUsed = count;
		fUsed = count * 2;
	}
	while (count > 0) {
		u_char data;
		u_short d;
		if (get_user(data, userPtr++))
			return -EFAULT;
		d = data;
		if (sound.hard.stereo) {
			// Translate stereo -> mono
			if (get_user(data, userPtr++))
				return -EFAULT;
			d += data;
			d >>= 1;	// take average of R/L ch
		}
		*p++ = d << 2;	// make it 10-bit
		count--;
	}
	*frameUsed += fUsed;
	// Clear rest of frame
	vr41xx_fill(0x0200, p, (frameLeft - fUsed) >> 1);

	return (uUsed);
}

static ssize_t vr41xx_ct_s16le(const u_char *userPtr, size_t userCount,
			 u_char frame[], ssize_t *frameUsed, ssize_t frameLeft)
{
	// uUsed & fUsed is byte count!!
	ssize_t count, uUsed, fUsed;
	u_short  *p = (u_short *)(&frame[*frameUsed]);

	if (sound.hard.stereo) {
		count = min(userCount >> 2, frameLeft >> 1);
		uUsed = count * 4;
		fUsed = count * 2;
	} else {
		count = min(userCount >> 1, frameLeft >> 1);
		uUsed = count * 2;
		fUsed = count * 2;
	}
	while (count > 0) {
		u_short data;
		u_int d;
		if (get_user(data, ((u_short *)userPtr)++))
			return -EFAULT;
		d = data ^ 0x8000;
		if (sound.hard.stereo) {
			if (get_user(data, ((u_short *)userPtr)++))
				return -EFAULT;
			d += data ^ 0x8000;
			d >>= 1;	// take average of R/L ch
		}
		*p++ = d >> 6;	// 16bit -> 10 bit
		count--;
	}
	*frameUsed += fUsed;
	// Clear rest of frame
	vr41xx_fill(0x0200, p, (frameLeft - fUsed) >> 1);

	return (uUsed);
}

static ssize_t vr41xx_ct_u16le(const u_char *userPtr, size_t userCount,
			 u_char frame[], ssize_t *frameUsed, ssize_t frameLeft)
{
	ssize_t count, uUsed, fUsed;
	u_short  *p = (u_short *)(&frame[*frameUsed]);

	if (sound.hard.stereo) {
		count = min(userCount >> 2, frameLeft >> 1);
		uUsed = count * 4;
		fUsed = count * 2;
	} else {
		count = min(userCount >> 1, frameLeft >> 1);
		uUsed = count * 2;
		fUsed = count * 2;
	}
	while (count > 0) {
		u_short data;
		u_int d;
		if (get_user(data, ((u_short *)userPtr)++))
			return -EFAULT;
		d = data;
		if (sound.hard.stereo) {
			if (get_user(data, ((u_short *)userPtr)++))
				return -EFAULT;
			d += data;
			d >>= 1;	// take average of R/L ch
		}
		*p++ = d >> 6;	// 16bit -> 10 bit
		count--;
	}
	*frameUsed += fUsed;
	// Clear rest of frame
	vr41xx_fill(0x0200, p, (frameLeft - fUsed) >> 1);

	return(uUsed);
}

/* Convert Big Endian short -> Little Endian short */
static inline u_short be2le(u_short d)
{
	return (((d << 8) & 0xff00) + ((d >>8) & 0x00ff));
}

static ssize_t vr41xx_ct_s16be(const u_char *userPtr, size_t userCount,
			 u_char frame[], ssize_t *frameUsed, ssize_t frameLeft)
{
	ssize_t count, uUsed, fUsed;
	u_short  *p = (u_short *)(&frame[*frameUsed]);

	if (sound.hard.stereo) {
		count = min(userCount >> 2, frameLeft >> 1);
		uUsed = count * 4;
		fUsed = count * 2;
	} else {
		count = min(userCount >> 1, frameLeft >> 1);
		uUsed = count * 2;
		fUsed = count * 2;
	}
	while (count > 0) {
		u_short data;
		u_int d;
		if (get_user(data, ((u_short *)userPtr)++))
			return -EFAULT;
		d = be2le(data) ^ 0x8000;
		if (sound.hard.stereo) {
			if (get_user(data, ((u_short *)userPtr)++))
				return -EFAULT;
			d += be2le(data) ^ 0x8000;
			d >>= 1;	// take average of R/L ch
		}
		*p++ = d >> 6;	// 16bit -> 10 bit
		count--;
	}
	*frameUsed += fUsed;
	// Clear rest of frame
	vr41xx_fill(0x0200, p, (frameLeft - fUsed) >> 1);

	return(uUsed);
}

static ssize_t vr41xx_ct_u16be(const u_char *userPtr, size_t userCount,
			 u_char frame[], ssize_t *frameUsed, ssize_t frameLeft)
{
	ssize_t count, uUsed, fUsed;
	u_short  *p = (u_short *)(&frame[*frameUsed]);

	if (sound.hard.stereo) {
		count = min(userCount >> 2, frameLeft >> 1);
		uUsed = count * 4;
		fUsed = count * 2;
	} else {
		count = min(userCount >> 1, frameLeft >> 1);
		uUsed = count * 2;
		fUsed = count * 2;
	}
	while (count > 0) {
		u_short data;
		u_int d;
		if (get_user(data, ((u_short *)userPtr)++))
			return -EFAULT;
		d = be2le(data);
		if (sound.hard.stereo) {
			if (get_user(data, ((u_short *)userPtr)++))
				return -EFAULT;
			d += be2le(data);
			d >>= 1;	// take average
		}
		*p++ = d >> 6;	// 16bit -> 10 bit
		count--;
	}
	*frameUsed += fUsed;
	// Clear rest of frame
	vr41xx_fill(0x0200, p, (frameLeft - fUsed) >> 1);

	return (uUsed);
}

#endif /* CONFIG_SOUND_VR41XX */

#ifdef CONFIG_SOUND_VR41XX
static TRANS transVr41xx = {
	vr41xx_ct_law, vr41xx_ct_law, vr41xx_ct_s8, vr41xx_ct_u8,
	vr41xx_ct_s16be, vr41xx_ct_u16be, vr41xx_ct_s16le, vr41xx_ct_u16le
};
#endif /* CONFIG_SOUND_VR41XX */

/*** Low level stuff *********************************************************/

static void vr41xx_set_rate(void)
{
	switch (sound.soft.speed) {
	case 8000:
		*VR41XX_SCNVRREG = 0x4;
		break;
	case 44100:
		*VR41XX_SCNVRREG = 0x2;
		break;
	case 22050:
		*VR41XX_SCNVRREG = 0x1;
		break;
	case 11025:
		*VR41XX_SCNVRREG = 0x0;
		break;
	default:
		*VR41XX_SCNVRREG = 0x4;	// default to 8K
		printk("DMA sound: Illegal sound rate %d\n", sound.soft.speed);
		break;
	}
}

static void vr41xx_enable_irq(void)
{
	// *VR41XX_MAIUINTREG = 0x000e;	// Enable Audio Output IRQs (All)
	*VR41XX_MAIUINTREG = 0x0008;	// Enable Audio Output IRQs (DMA page 2)
	// *VR41XX_MAIUINTREG = 0x0004;	// Enable Audio Output IRQs (DMA page 1)
	barrier();
	enable_irq(VR41XX_IRQ_AIU);
}

static void vr41xx_disable_irq(void)
{
	disable_irq(VR41XX_IRQ_AIU);
	barrier();
	*VR41XX_MAIUINTREG = 0x0000;	// Disable Audio Output IRQs
}

static void vr41xx_clear_irq(void)
{
	// fixme
	*VR41XX_INTREG = 0x000e;	// Clear All Audio Output IRQs
	barrier();
}

static void vr41xx_enable_dma(void)
{
	// Enable DMA in DCU
	*VR41XX_DMARSTREG |= 0x0001;	// Release DMA Reset
	barrier();
	*VR41XX_DMAMSKREG |= 0x0004;	// Enable Audio Output DMA
	barrier();
	*VR41XX_DMASENREG |= 0x0001;	// Enable DMA Sequencer
	barrier();
	// But DMA not started yet, need 'AUISEN := 1'
}

static void vr41xx_disable_dma(void)
{
	*VR41XX_DMAMSKREG &= 0xfffb;	// Disable Audio Output DMA
	barrier();
}

static void vr41xx_suspend_dma(void)
{
	*VR41XX_DMAMSKREG &= 0xfffb;	// Disable Audio Output DMA
}

static void vr41xx_resume_dma(void)
{
	*VR41XX_DMAMSKREG |= 0x0004;	// Enable Audio Output DMA
}

static void vr41xx_speaker_on(void)
{
    /* Speaker power ON via GPIO it's machine dependent */
	VR41XX_ENABLE_SPEAKER();
	barrier();
}

static void vr41xx_disable_sound(void)
{
	vr41xx_disable_dma();
	vr41xx_clear_irq();
	// vr41xx_disable_irq();

    /* Speaker operation disable */
    *VR41XX_SEQREG  = 0x0000;	// AUISEN := 0
    barrier();

    /* Speaker power OFF via GPIO it's machine dependent */
	VR41XX_DISABLE_SPEAKER();
	barrier();

    /* DAC vref Off */
    *VR41XX_SCNTREG &= 0x7fff;
    barrier();

    /* Disable AIU clock in CMU */
    *VR41XX_CMUCLKMSK &= 0xfffb;
    barrier();
}

		

static void vr41xx_kick_dma(void)
{
    /* conversion rate */
	vr41xx_set_rate();

	/* Enable DMA */
	vr41xx_enable_dma();

    /* DAC Vref on */
	*VR41XX_SCNTREG |= 0x8000;
    barrier();

	/* Wait for Vref stablization */
	// udelay(10);
	mdelay(50);	// 50 msec is enough ?

	// Speaker Power On
	vr41xx_speaker_on();

    // Speaker operation enable
    *VR41XX_SEQREG  = 0x0001;	// AUISEN := 1
    barrier();
}


/*
 * VR41XX
 */
static int __init Vr41xxIrqInit(void)
{
	int err;
	/* turn off DMA for audio channels */
	vr41xx_disable_dma();
	vr41xx_clear_irq();
	vr41xx_disable_sound();
	/* Register interrupt handler. */
	if ((err = request_irq(VR41XX_IRQ_AIU, vr41xx_sq_interrupt, 0,
			"DMA sound", vr41xx_sq_interrupt))) {
		return(0);
	}
	return(1);
}

#ifdef MODULE
static void Vr41xxIrqCleanUp(void)
{
	/* turn off DMA for audio channels */
	vr41xx_disable_dma();
	vr41xx_disable_irq();
	vr41xx_clear_irq();
	vr41xx_disable_sound();
	/* release the interrupt */
	free_irq(VR41XX_IRQ_AIU, vr41xx_sq_interrupt);
}
#endif /* MODULE */

#ifdef CONFIG_SOUND_VR41XX
static void Vr41xxSilence(void)
{
	/* Disable sound & DMA */
	vr41xx_disable_sound();
	vr41xx_disable_irq();
}
#else
static void AmiSilence(void)
{
	/* turn off DMA for audio channels */
	custom.dmacon = AMI_AUDIO_OFF;
}
#endif /* CONFIG_SOUND_VR41XX */


#ifdef CONFIG_SOUND_VR41XX
static char vr41xx_first_dma_kick = 1;

static void Vr41xxInit(void)
{
	sound.hard = sound.soft;
	sound.trans = &transVr41xx;

	vr41xx_first_dma_kick = 1;

    /* Enable AIU clock in CMU */
	*VR41XX_CMUCLKMSK |= 0x0004;
    barrier();
	vr41xx_enable_irq();

}
#else
static void AmiInit(void)
{
	int period, i;

	AmiSilence();

	if (sound.soft.speed)
		period = amiga_colorclock/sound.soft.speed-1;
	else
		period = amiga_audio_min_period;
	sound.hard = sound.soft;
	sound.trans = &transAmiga;

	if (period < amiga_audio_min_period) {
		/* we would need to squeeze the sound, but we won't do that */
		period = amiga_audio_min_period;
	} else if (period > 65535) {
		period = 65535;
	}
	sound.hard.speed = amiga_colorclock/(period+1);

	for (i = 0; i < 4; i++)
		custom.aud[i].audper = period;
	amiga_audio_period = period;

	AmiSetTreble(50);  /* recommended for newer amiga models */
}
#endif /* CONFIG_SOUND_VR41XX */


#ifdef CONFIG_SOUND_VR41XX
static int Vr41xxSetFormat(int format)
{
	int size;

	/* Vr41xx sound DMA supports only 8bit (expanded to 10bit mode */
	switch (format) {
	case AFMT_QUERY:
		return(sound.soft.format);
	case AFMT_MU_LAW:
		size = 8;
		break;
	case AFMT_A_LAW:
		size = 8;
		break;
	case AFMT_U8:
		size = 8;
		break;
	case AFMT_S8:
		size = 8;
		break;
	case AFMT_S16_BE:
		size = 16;
		break;
	case AFMT_U16_BE:
		size = 16;
		break;
	case AFMT_S16_LE:
		size = 16;
		break;
	case AFMT_U16_LE:
		size = 16;
		break;
	default: /* :-) */
#ifdef DEBUG
		printk("default: AFMT_S8\n");
#endif
		size = 8;
		format = AFMT_S8;
	}

	sound.soft.format = format;
	sound.soft.size = size;
	if (sound.minDev == SND_DEV_DSP) {
		sound.dsp.format = format;
		sound.dsp.size = sound.soft.size;
	}

	return(format);
}
#else
static int AmiSetFormat(int format)
{
	int size;

	/* Amiga sound DMA supports 8bit and 16bit (pseudo 14 bit) modes */

	switch (format) {
	case AFMT_QUERY:
		return(sound.soft.format);
	case AFMT_MU_LAW:
	case AFMT_A_LAW:
	case AFMT_U8:
	case AFMT_S8:
		size = 8;
		break;
	case AFMT_S16_BE:
	case AFMT_U16_BE:
	case AFMT_S16_LE:
	case AFMT_U16_LE:
		size = 16;
		break;
	default: /* :-) */
		size = 8;
		format = AFMT_S8;
	}

	sound.soft.format = format;
	sound.soft.size = size;
	if (sound.minDev == SND_DEV_DSP) {
		sound.dsp.format = format;
		sound.dsp.size = sound.soft.size;
	}
	AmiInit();

	return(format);
}
#endif /* CONFIG_SOUND_VR41XX */


#ifdef CONFIG_SOUND_VR41XX
#else
#define VOLUME_VOXWARE_TO_AMI(v) \
	(((v) < 0) ? 0 : ((v) > 100) ? 64 : ((v) * 64)/100)
#define VOLUME_AMI_TO_VOXWARE(v) ((v)*100/64)

static int AmiSetVolume(int volume)
{
	sound.volume_left = VOLUME_VOXWARE_TO_AMI(volume & 0xff);
	custom.aud[0].audvol = sound.volume_left;
	sound.volume_right = VOLUME_VOXWARE_TO_AMI((volume & 0xff00) >> 8);
	custom.aud[1].audvol = sound.volume_right;
	return(VOLUME_AMI_TO_VOXWARE(sound.volume_left) |
	       (VOLUME_AMI_TO_VOXWARE(sound.volume_right) << 8));
}

static int AmiSetTreble(int treble)
{
	sound.treble = treble;
	if (treble < 50)
		ciaa.pra &= ~0x02;
	else
		ciaa.pra |= 0x02;
	return(treble);
}
#endif /* CONFIG_SOUND_VR41XX */


#ifdef CONFIG_SOUND_VR41XX

#define VR41XX_PLAY_LOADED		1
#define VR41XX_PLAY_PLAYING		2
#define VR41XX_PLAY_MASK		3


static void vr41xx_sq_play_next_frame(int index)
{
	u_char *start;
	u_long size;
	u_int ladr, hadr;


	/* used by Vr41xxPlay() if all doubts whether there really is something
	 * to be played are already wiped out.
	 */
	start = (u_char *)sq_block_address(sq.front);
	size = (sq.count == index ? sq.rear_size : sq.block_size)>>1;

	// Setup DMA address
	ladr = (u_int)start & 0xffff;
	// Mask upper 7bit (only lower 32M byte can set to DMA adr reg.)
	hadr = ((u_int)start >> 16) & 0x01ff;
	*VR41XX_AIUOBALREG = ladr;
	*VR41XX_AIUOBAHREG = hadr;
	barrier();
	if (vr41xx_first_dma_kick) {
		// On first DMA, DMA address reg & DMA base reg should be set
		*VR41XX_AIUOALREG  = ladr;
		*VR41XX_AIUOAHREG  = hadr;
	}
	vr41xx_clear_irq();
	sq.front = (sq.front+1) % sq.max_count;
	sq.active |= VR41XX_PLAY_LOADED;

	vr41xx_resume_dma();

	if (vr41xx_first_dma_kick) {
		vr41xx_kick_dma();
		vr41xx_first_dma_kick = 0;
	}
}

#else

static void ami_sq_play_next_frame(int index)
{
	u_char *start, *ch0, *ch1, *ch2, *ch3;
	u_long size;

	/* used by AmiPlay() if all doubts whether there really is something
	 * to be played are already wiped out.
	 */
	start = sq_block_address(sq.front);
	size = (sq.count == index ? sq.rear_size : sq.block_size)>>1;

	if (sound.hard.stereo) {
		ch0 = start;
		ch1 = start+sq.block_size_half;
		size >>= 1;
	} else {
		ch0 = start;
		ch1 = start;
	}
	if (sound.hard.size == 8) {
		custom.aud[0].audlc = (u_short *)ZTWO_PADDR(ch0);
		custom.aud[0].audlen = size;
		custom.aud[1].audlc = (u_short *)ZTWO_PADDR(ch1);
		custom.aud[1].audlen = size;
		custom.dmacon = AMI_AUDIO_8;
	} else {
		size >>= 1;
		custom.aud[0].audlc = (u_short *)ZTWO_PADDR(ch0);
		custom.aud[0].audlen = size;
		custom.aud[1].audlc = (u_short *)ZTWO_PADDR(ch1);
		custom.aud[1].audlen = size;
		if (sound.volume_left == 64 && sound.volume_right == 64) {
			/* We can play pseudo 14-bit only with the maximum volume */
			ch3 = ch0+sq.block_size_quarter;
			ch2 = ch1+sq.block_size_quarter;
			custom.aud[2].audvol = 1;  /* we are being affected by the beeps */
			custom.aud[3].audvol = 1;  /* restoring volume here helps a bit */
			custom.aud[2].audlc = (u_short *)ZTWO_PADDR(ch2);
			custom.aud[2].audlen = size;
			custom.aud[3].audlc = (u_short *)ZTWO_PADDR(ch3);
			custom.aud[3].audlen = size;
			custom.dmacon = AMI_AUDIO_14;
		} else
			custom.dmacon = AMI_AUDIO_8;
	}
	sq.front = (sq.front+1) % sq.max_count;
	sq.active |= AMI_PLAY_LOADED;
}
#endif /* CONFIG_SOUND_VR41XX */


#ifdef CONFIG_SOUND_VR41XX
static void Vr41xxPlay(void)
{
	// fixme
	int minframes = 1;
	vr41xx_disable_irq();

	if (sq.active & VR41XX_PLAY_LOADED) {
		/* There's already a frame loaded */
		vr41xx_enable_irq();
		return;
	}

	if (sq.active & VR41XX_PLAY_PLAYING)
		/* Increase threshold: frame 1 is already being played */
		minframes = 2;

	if (sq.count < minframes) {
		/* Nothing to do */
		vr41xx_enable_irq();
		return;
	}
	if (sq.count <= minframes && sq.rear_size < sq.block_size && !sq.syncing) {
		/* hmmm, the only existing frame is not
		 * yet filled and we're not syncing?
		 */
		vr41xx_enable_irq();
		return;
	}

	vr41xx_sq_play_next_frame(minframes);

	vr41xx_enable_irq();
}
#else
static void AmiPlay(void)
{
	int minframes = 1;

	custom.intena = IF_AUD0;

	if (sq.active & AMI_PLAY_LOADED) {
		/* There's already a frame loaded */
		custom.intena = IF_SETCLR | IF_AUD0;
		return;
	}

	if (sq.active & AMI_PLAY_PLAYING)
		/* Increase threshold: frame 1 is already being played */
		minframes = 2;

	if (sq.count < minframes) {
		/* Nothing to do */
		custom.intena = IF_SETCLR | IF_AUD0;
		return;
	}

	if (sq.count <= minframes && sq.rear_size < sq.block_size && !sq.syncing) {
		/* hmmm, the only existing frame is not
		 * yet filled and we're not syncing?
		 */
		custom.intena = IF_SETCLR | IF_AUD0;
		return;
	}

	ami_sq_play_next_frame(minframes);

	custom.intena = IF_SETCLR | IF_AUD0;
}
#endif /* CONFIG_SOUND_VR41XX */


#ifdef CONFIG_SOUND_VR41XX
static void vr41xx_sq_interrupt(int irq, void *dummy, struct pt_regs *fp)
{
	int minframes = 1;

	// vr41xx_clear_irq();

	if (!sq.active) {
		/* Playing was interrupted and sq_reset() has already cleared
		 * the sq variables, so better don't do anything here.
		 */
		WAKE_UP(sq.sync_queue);
		vr41xx_clear_irq();
		return;
	}

	if (sq.active & VR41XX_PLAY_PLAYING) {
		/* We've just finished a frame */
		sq.count--;
		if (sq.count < sq.wake_up_threshold)
			WAKE_UP(sq.action_queue);
	}

	if (sq.active & VR41XX_PLAY_LOADED)
		/* Increase threshold: frame 1 is already being played */
		minframes = 2;

	/* Shift the flags */
	sq.active = (sq.active << 1) & VR41XX_PLAY_MASK;

	if (!sq.active) {
		/* No frame is playing, disable audio DMA */
		vr41xx_clear_irq();
/* 000315 kei -- to avoid kernel hang up when heavy load...
		vr41xx_suspend_dma();
*/
	}

	if (sq.count >= minframes) {
		/* Try to play the next frame */
		Vr41xxPlay();
	}

	if (!sq.active) {
		/* Nothing to play anymore.
		   Wake up a process waiting for audio output to drain. */
		WAKE_UP(sq.sync_queue);
	}
}

#else

static void ami_sq_interrupt(int irq, void *dummy, struct pt_regs *fp)
{
	int minframes = 1;

	if (!sq.active) {
		/* Playing was interrupted and sq_reset() has already cleared
		 * the sq variables, so better don't do anything here.
		 */
		WAKE_UP(sq.sync_queue);
		return;
	}

	if (sq.active & AMI_PLAY_PLAYING) {
		/* We've just finished a frame */
		sq.count--;
		WAKE_UP(sq.action_queue);
	}

	if (sq.active & AMI_PLAY_LOADED)
		/* Increase threshold: frame 1 is already being played */
		minframes = 2;

	/* Shift the flags */
	sq.active = (sq.active<<1) & AMI_PLAY_MASK;

	if (!sq.active)
		/* No frame is playing, disable audio DMA */
		custom.dmacon = AMI_AUDIO_OFF;

	if (sq.count >= minframes) {
		/* Try to play the next frame */
		Vr41xxPlay();
	}

	if (!sq.active)
		/* Nothing to play anymore.
		   Wake up a process waiting for audio output to drain. */
		WAKE_UP(sq.sync_queue);
}
#endif /* CONFIG_SOUND_VR41XX */


/*** Machine definitions *****************************************************/

#ifdef CONFIG_SOUND_VR41XX
static MACHINE machVr41xx = {
	DMASND_VR41XX,		// int type
	NULL,				// void *dma_alloc(uint, int)
	NULL,				// void  dma_free(void *, uint)
	Vr41xxIrqInit,		// void  irqinit(void)
#ifdef MODULE
	Vr41xxIrqCleanUp,	// void  irqcleanup(void)
#endif /* MODULE */
	Vr41xxInit,			// void  init(void)
	Vr41xxSilence,		// void  silence(void)
	Vr41xxSetFormat,	// int   setFormat(int)
	NULL,				// int   setVolume(int)
	NULL,				// int   setBass(int)
	NULL,				// int   setTreble(int)
	NULL,				// int   setGain(int)
	Vr41xxPlay			// void  play(void)
};
#endif /* CONFIG_SOUND_VR41XX */


/*** Mid level stuff *********************************************************/


static void sound_silence(void)
{
	/* update hardware settings one more */
	(*sound.mach.init)();

	(*sound.mach.silence)();
}


static void sound_init(void)
{
	(*sound.mach.init)();
}


static int sound_set_format(int format)
{
	return(*sound.mach.setFormat)(format);
}


static int sound_set_speed(int speed)
{
	if (speed < 0)
		return(sound.soft.speed);

	sound.soft.speed = speed;
	(*sound.mach.init)();
	if (sound.minDev == SND_DEV_DSP)
		sound.dsp.speed = sound.soft.speed;

	return(sound.soft.speed);
}


static int sound_set_stereo(int stereo)
{
	if (stereo < 0)
		return(sound.soft.stereo);
	stereo = !!stereo;    /* should be 0 or 1 now */

	sound.soft.stereo = stereo;
	if (sound.minDev == SND_DEV_DSP)
		sound.dsp.stereo = stereo;
	(*sound.mach.init)();

	return(stereo);
}

#ifdef CONFIG_SOUND_VR41XX
	// fixme
#else
static int sound_set_volume(int volume)
{
	return(*sound.mach.setVolume)(volume);
}
#endif


static ssize_t sound_copy_translate(const u_char *userPtr,
				    size_t userCount,
				    u_char frame[], ssize_t *frameUsed,
				    ssize_t frameLeft)
{
	ssize_t
		(*ct_func)(const u_char *, size_t, u_char *, ssize_t *, ssize_t) = NULL;

	switch (sound.soft.format) {
	case AFMT_MU_LAW:
		ct_func = sound.trans->ct_ulaw;
		break;
	case AFMT_A_LAW:
		ct_func = sound.trans->ct_alaw;
		break;
	case AFMT_S8:
		ct_func = sound.trans->ct_s8;
		break;
	case AFMT_U8:
		ct_func = sound.trans->ct_u8;
		break;
	case AFMT_S16_BE:
		ct_func = sound.trans->ct_s16be;
		break;
	case AFMT_U16_BE:
		ct_func = sound.trans->ct_u16be;
		break;
	case AFMT_S16_LE:
		ct_func = sound.trans->ct_s16le;
		break;
	case AFMT_U16_LE:
		ct_func = sound.trans->ct_u16le;
		break;
	}
	if (ct_func)
		return ct_func(userPtr, userCount, frame, frameUsed, frameLeft);
	else
		return 0;
}


/*
 * /dev/mixer abstraction
 */


#define RECLEVEL_VOXWARE_TO_GAIN(v) \
	((v) < 0 ? 0 : (v) > 100 ? 15 : (v) * 3 / 20)
#define RECLEVEL_GAIN_TO_VOXWARE(v) (((v) * 20 + 2) / 3)


static int mixer_open(struct inode *inode, struct file *file)
{
	MOD_INC_USE_COUNT;
	mixer.busy = 1;
	return 0;
}


static int mixer_release(struct inode *inode, struct file *file)
{
	mixer.busy = 0;
	MOD_DEC_USE_COUNT;
#ifdef CONFIG_SOUND_VR41XX
	vr41xx_disable_sound();
#endif
	return 0;
}


static int mixer_ioctl(struct inode *inode, struct file *file, u_int cmd,
		       u_long arg)
{
	// int data;
	if (_SIOC_DIR(cmd) & _SIOC_WRITE)
	    mixer.modify_counter++;
	if (cmd == OSS_GETVERSION)
	    return IOCTL_OUT(arg, SOUND_VERSION);
	switch (sound.mach.type) {
#ifdef CONFIG_SOUND_VR41XX
	case DMASND_VR41XX:
		switch (cmd) {
		case SOUND_MIXER_INFO: {
		    mixer_info info;
		    strncpy(info.id, "VR41XX", sizeof(info.id));
		    strncpy(info.name, "VR41XX", sizeof(info.name));
		    info.name[sizeof(info.name)-1] = 0;
		    info.modify_counter = mixer.modify_counter;
		    if (copy_to_user((int *)arg, &info, sizeof(info)))
			    return -EFAULT;
		    return 0;
		}
		case SOUND_MIXER_READ_DEVMASK:
			return IOCTL_OUT(arg, 0);	// fixme
		case SOUND_MIXER_READ_RECMASK:
			return IOCTL_OUT(arg, 0);
		case SOUND_MIXER_READ_STEREODEVS:
			return IOCTL_OUT(arg, 0);
		case SOUND_MIXER_READ_VOLUME:
			return IOCTL_OUT(arg, 0);	// fixme
		case SOUND_MIXER_WRITE_VOLUME:
			return IOCTL_OUT(arg, 0);	// fixme
		case SOUND_MIXER_READ_TREBLE:
			return IOCTL_OUT(arg, 0);	// fixme
		case SOUND_MIXER_WRITE_TREBLE:
			return IOCTL_OUT(arg, 0);	// fixme
		}
		break;
#endif /* CONFIG_SOUND_VR41XX */
	}

	return -EINVAL;
}


static struct file_operations mixer_fops =
{
	llseek:  sound_lseek,
	ioctl:   mixer_ioctl,
	open:    mixer_open,
	release: mixer_release
};

static void __init mixer_init(void)
{
#ifndef MODULE
	int mixer_unit;
#endif
	mixer_unit = register_sound_mixer(&mixer_fops, -1);
	if (mixer_unit < 0)
		return;

	mixer.busy = 0;
	sound.treble = 0;
	sound.bass = 0;
	switch (sound.mach.type) {
    case DMASND_VR41XX:
		sound.volume_left = 64;
		sound.volume_right = 64;
		// nothing to do ?? fixme
		break;
	}
}


/*
 * Sound queue stuff, the heart of the driver
 */


static int sq_allocate_buffers(void)
{
	int i;

	if (sound_buffers)
		return 0;
	sound_buffers = kmalloc (numBufs * sizeof(char *), GFP_DMA);
	if (!sound_buffers)
		return -ENOMEM;
#ifdef CONFIG_SOUND_VR41XX
	vr41xx_dma_bufs = kmalloc(numBufs * sizeof(char *), GFP_DMA);
	if (!vr41xx_dma_bufs) {
		kfree(sound_buffers);
		sound_buffers = 0;
		return -ENOMEM;
	}
	for (i = 0; i < numBufs; i++) {
		/* need to keep 2K byte boundary */
		/* First, try allocating 2K byte */
		vr41xx_dma_bufs[i] = kmalloc(2 * 1024, GFP_DMA);
		if (vr41xx_dma_bufs[i] == NULL) {
			// Fail to allocate
			while (i--)
				kfree(vr41xx_dma_bufs[i]);
			kfree(sound_buffers);
			kfree(vr41xx_dma_bufs);
			sound_buffers = vr41xx_dma_bufs = 0;
			return -ENOMEM;
		} else {
			if (((u_long)vr41xx_dma_bufs[i] & 0x7ff) == 0) {
				// Good, allocated on 2K byte boundary
				// Map to KSEG1
				sound_buffers[i] = (char *)
					((u_long)vr41xx_dma_bufs[i] | KSEG1);
			} else {
				// not allocated on 2K byte boundary
				// So, try allocating 4K byte
				kfree(vr41xx_dma_bufs[i]);
				vr41xx_dma_bufs[i] = kmalloc(4 * 1024, GFP_DMA);
				/*
					1) round to 2K byte boundary
					2) Map to KSEG1 (not TLB mapped, not cached)
					   (Because DMA access these area)
				*/
				sound_buffers[i] = (char *)
				((((u_long)vr41xx_dma_bufs[i] & 0xfffff800) | KSEG1) + 0x0800);
			}
		}
	}
#else
	for (i = 0; i < numBufs; i++) {
		sound_buffers[i] = sound.mach.dma_alloc (bufSize << 10, GFP_KERNEL);
		if (!sound_buffers[i]) {
			while (i--)
				sound.mach.dma_free (sound_buffers[i], bufSize << 10);
			kfree (sound_buffers);
			sound_buffers = 0;
			return -ENOMEM;
		}
	}
#endif
	return 0;
}


static void sq_release_buffers(void)
{
	int i;

#ifdef CONFIG_SOUND_VR41XX
	if (vr41xx_dma_bufs) {
		for (i = 0; i < numBufs; i++)
			kfree(vr41xx_dma_bufs[i]);
		kfree (sound_buffers);
		kfree (vr41xx_dma_bufs);
		sound_buffers = vr41xx_dma_bufs = 0;
	}
#else
	if (sound_buffers) {
		for (i = 0; i < numBufs; i++)
			sound.mach.dma_free (sound_buffers[i], bufSize << 10);
		kfree (sound_buffers);
		sound_buffers = 0;
	}
#endif
}

static void sq_setup(int numBufs, int bufSize, char **write_buffers)
{
	sq.max_count = numBufs;
	sq.max_active = numBufs;
	sq.block_size = bufSize;
	sq.buffers = write_buffers;

	sq.front = sq.count = 0;
	sq.rear = -1;
	sq.syncing = 0;
	sq.active = 0;
#ifdef CONFIG_SOUND_VR41XX
	sq.wake_up_threshold = numBufs / 4;
#endif
}



static void sq_play(void)
{
	(*sound.mach.play)();
}


/* ++TeSche: radically changed this one too */

static ssize_t sq_write(struct file *file, const char *src, size_t uLeft,
			loff_t *ppos)
{
	ssize_t uWritten = 0;
	u_char *dest;
	ssize_t uUsed, bUsed, bLeft;

	/* ++TeSche: Is something like this necessary?
	 * Hey, that's an honest question! Or does any other part of the
	 * filesystem already checks this situation? I really don't know.
	 */
	if (uLeft == 0)
		return 0;

	/* The interrupt doesn't start to play the last, incomplete frame.
	 * Thus we can append to it without disabling the interrupts! (Note
	 * also that sq.rear isn't affected by the interrupt.)
	 */
	if (sq.count > 0 && (bLeft = sq.block_size-sq.rear_size) > 0) {
		dest = sq_block_address(sq.rear);
		bUsed = sq.rear_size;
		uUsed = sound_copy_translate(src, uLeft, dest, &bUsed, bLeft);
		if (uUsed <= 0)
			return uUsed;
		src += uUsed;
		uWritten += uUsed;
		uLeft -= uUsed;
		sq.rear_size = bUsed;
	}

	do {
		while (sq.count == sq.max_active) {
			/* All frames are in use.  -- kei */
			sq_play();
			if (NON_BLOCKING(sq.open_mode))
				return uWritten > 0 ? uWritten : -EAGAIN;
			SLEEP(sq.action_queue, ONE_SECOND);
			if (SIGNAL_RECEIVED)
				return uWritten > 0 ? uWritten : -EINTR;
		}

		/* Here, we can avoid disabling the interrupt by first
		 * copying and translating the data, and then updating
		 * the sq variables. Until this is done, the interrupt
		 * won't see the new frame and we can work on it
		 * undisturbed.
		 */

		dest = sq_block_address((sq.rear+1) % sq.max_count);
		bUsed = 0;
		bLeft = sq.block_size;
		uUsed = sound_copy_translate(src, uLeft, dest, &bUsed, bLeft);
		if (uUsed <= 0)
			break;
		src += uUsed;
		uWritten += uUsed;
		uLeft -= uUsed;
		if (bUsed) {
			// Need to be atomic -- kei
			disable_irq(VR41XX_IRQ_AIU);
			sq.rear = (sq.rear+1) % sq.max_count;
			sq.rear_size = bUsed;
			sq.count++;
			enable_irq(VR41XX_IRQ_AIU);
		}
	} while (bUsed);   /* uUsed may have been 0 */
	sq_play();
	return uUsed < 0? uUsed: uWritten;
}

/***********/


static int sq_open(struct inode *inode, struct file *file)
{
	int rc = 0;

	MOD_INC_USE_COUNT;
	if (file->f_mode & FMODE_WRITE) {
		if (sq.busy) {
			rc = -EBUSY;
			if (NON_BLOCKING(file->f_flags))
				goto err_out;
			rc = -EINTR;
			while (sq.busy) {
				SLEEP(sq.open_queue, ONE_SECOND);
				if (SIGNAL_RECEIVED)
					goto err_out;
			}
		}
		sq.busy = 1; /* Let's play spot-the-race-condition */

		if (sq_allocate_buffers()) goto err_out_nobusy;

		sq_setup(numBufs, bufSize<<10,sound_buffers);
		sq.open_mode = file->f_mode;
	}
	sound.minDev = MINOR(inode->i_rdev) & 0x0f;
	sound.soft = sound.dsp;
	sound.hard = sound.dsp;
	sound_init();
	if ((MINOR(inode->i_rdev) & 0x0f) == SND_DEV_AUDIO) {
		sound_set_speed(8000);
		sound_set_stereo(0);
		sound_set_format(AFMT_MU_LAW);
	}

	return 0;

err_out_nobusy:
	if (file->f_mode & FMODE_WRITE) {
		sq.busy = 0;
		WAKE_UP(sq.open_queue);
	}
err_out:
	MOD_DEC_USE_COUNT;
	return rc;
}


static void sq_reset(void)
{
	sound_silence();
	sq.active = 0;
	sq.count = 0;
	sq.front = (sq.rear+1) % sq.max_count;
}


static int sq_fsync(struct file *filp, struct dentry *dentry)
{
	int rc = 0;

	sq.syncing = 1;
	sq_play();	/* there may be an incomplete frame waiting */

	while (sq.active) {
		SLEEP(sq.sync_queue, ONE_SECOND);
		if (SIGNAL_RECEIVED) {
			/* While waiting for audio output to drain, an
			 * interrupt occurred.  Stop audio output immediately
			 * and clear the queue. */
			sq_reset();
			rc = -EINTR;
			break;
		}
	}

	sq.syncing = 0;
	return rc;
}

static int sq_release(struct inode *inode, struct file *file)
{
	int rc = 0;

	if (sq.busy)
		rc = sq_fsync(file, file->f_dentry);
	sound.soft = sound.dsp;
	sound.hard = sound.dsp;
	sound_silence();

	sq_release_buffers();
	MOD_DEC_USE_COUNT;

	/* There is probably a DOS atack here. They change the mode flag. */
	/* XXX add check here */
	if (file->f_mode & FMODE_WRITE) {
		sq.busy = 0;
		WAKE_UP(sq.open_queue);
	}

	/* Wake up a process waiting for the queue being released.
	 * Note: There may be several processes waiting for a call
	 * to open() returning. */

	return rc;
}


static int sq_ioctl(struct inode *inode, struct file *file, u_int cmd,
		    u_long arg)
{
	u_long fmt;
	int data;
	int size, nbufs;

	switch (cmd) {
	case SNDCTL_DSP_RESET:
		sq_reset();
		return 0;
	case SNDCTL_DSP_POST:
	case SNDCTL_DSP_SYNC:
		return sq_fsync(file, file->f_dentry);

		/* ++TeSche: before changing any of these it's
		 * probably wise to wait until sound playing has
		 * settled down. */
	case SNDCTL_DSP_SPEED:
		sq_fsync(file, file->f_dentry);
		IOCTL_IN(arg, data);
		return IOCTL_OUT(arg, sound_set_speed(data));
	case SNDCTL_DSP_STEREO:
		sq_fsync(file, file->f_dentry);
		IOCTL_IN(arg, data);
		return IOCTL_OUT(arg, sound_set_stereo(data));
	case SOUND_PCM_WRITE_CHANNELS:
		sq_fsync(file, file->f_dentry);
		IOCTL_IN(arg, data);
		return IOCTL_OUT(arg, sound_set_stereo(data-1)+1);
	case SNDCTL_DSP_SETFMT:
		sq_fsync(file, file->f_dentry);
		IOCTL_IN(arg, data);
		return IOCTL_OUT(arg, sound_set_format(data));
	case SNDCTL_DSP_GETFMTS:
		fmt = 0;
		if (sound.trans) {
			if (sound.trans->ct_ulaw)
				fmt |= AFMT_MU_LAW;
			if (sound.trans->ct_alaw)
				fmt |= AFMT_A_LAW;
			if (sound.trans->ct_s8)
				fmt |= AFMT_S8;
			if (sound.trans->ct_u8)
				fmt |= AFMT_U8;
			if (sound.trans->ct_s16be)
				fmt |= AFMT_S16_BE;
			if (sound.trans->ct_u16be)
				fmt |= AFMT_U16_BE;
			if (sound.trans->ct_s16le)
				fmt |= AFMT_S16_LE;
			if (sound.trans->ct_u16le)
				fmt |= AFMT_U16_LE;
		}
		return IOCTL_OUT(arg, fmt);
	case SNDCTL_DSP_GETBLKSIZE:
		size = sq.block_size
			* sound.soft.size * (sound.soft.stereo + 1)
			/ (sound.hard.size * (sound.hard.stereo + 1));
		return IOCTL_OUT(arg, size);
	case SNDCTL_DSP_SUBDIVIDE:
		break;
	case SNDCTL_DSP_SETFRAGMENT:
		if (sq.count || sq.active || sq.syncing)
			return -EINVAL;
#ifdef CONFIG_SOUND_VR41XX
		// Can't set fragment on VR41xx Sound driver
		return -EINVAL;
#endif
		IOCTL_IN(arg, size);
		nbufs = size >> 16;
		if (nbufs < 2 || nbufs > numBufs)
			nbufs = numBufs;
		size &= 0xffff;
		if (size >= 8 && size <= 29) {
			size = 1 << size;
			size *= sound.hard.size * (sound.hard.stereo + 1);
			size /= sound.soft.size * (sound.soft.stereo + 1);
			if (size > (bufSize << 10))
				size = bufSize << 10;
		} else
			size = bufSize << 10;
		sq_setup(numBufs, size, sound_buffers);
		sq.max_active = nbufs;
		return 0;

	default:
		return mixer_ioctl(inode, file, cmd, arg);
	}
	return -EINVAL;
}



static struct file_operations sq_fops =
{
	llseek:  sound_lseek,
	write:   sq_write,
	ioctl:   sq_ioctl,
	open:    sq_open,
	release: sq_release
};


static void __init sq_init(void)
{
#ifndef MODULE
	int sq_unit;
#endif
	sq_unit = register_sound_dsp(&sq_fops, -1);
	if (sq_unit < 0)
		return;

	init_waitqueue_head(&sq.action_queue);
	init_waitqueue_head(&sq.open_queue);
	init_waitqueue_head(&sq.sync_queue);

	sq.busy = 0;

	/* whatever you like as startup mode for /dev/dsp,
	 * (/dev/audio hasn't got a startup mode). note that
	 * once changed a new open() will *not* restore these!
	 */
	sound.dsp.format = AFMT_U8;
	sound.dsp.stereo = 0;
	sound.dsp.size = 8;

	/* set minimum rate possible without expanding */
	switch (sound.mach.type) {
	case DMASND_VR41XX:
		sound.dsp.speed = 8000;
		break;
	}

	/* before the first open to /dev/dsp this wouldn't be set */
	sound.soft = sound.dsp;
	sound.hard = sound.dsp;

	sound_silence();
}

/*
 * /dev/sndstat
 */


/* state.buf should not overflow! */

static int state_open(struct inode *inode, struct file *file)
{
	char *buffer = state.buf, *mach = "";
	int len = 0;

	if (state.busy)
		return -EBUSY;

	MOD_INC_USE_COUNT;
	state.ptr = 0;
	state.busy = 1;

	switch (sound.mach.type) {
	case DMASND_VR41XX:
		mach = "Vr41xx ";
		break;
	}
	len += sprintf(buffer+len, "%sDMA sound driver:\n", mach);

	len += sprintf(buffer+len, "\tsound.format = 0x%x", sound.soft.format);
	switch (sound.soft.format) {
	case AFMT_MU_LAW:
		len += sprintf(buffer+len, " (mu-law)");
		break;
	case AFMT_A_LAW:
		len += sprintf(buffer+len, " (A-law)");
		break;
	case AFMT_U8:
		len += sprintf(buffer+len, " (unsigned 8 bit)");
		break;
	case AFMT_S8:
		len += sprintf(buffer+len, " (signed 8 bit)");
		break;
	case AFMT_S16_BE:
		len += sprintf(buffer+len, " (signed 16 bit big)");
		break;
	case AFMT_U16_BE:
		len += sprintf(buffer+len, " (unsigned 16 bit big)");
		break;
	case AFMT_S16_LE:
		len += sprintf(buffer+len, " (signed 16 bit little)");
		break;
	case AFMT_U16_LE:
		len += sprintf(buffer+len, " (unsigned 16 bit little)");
		break;
	}
	len += sprintf(buffer+len, "\n");
	len += sprintf(buffer+len, "\tsound.speed = %dHz (phys. %dHz)\n",
		       sound.soft.speed, sound.hard.speed);
	len += sprintf(buffer+len, "\tsound.stereo = 0x%x (%s)\n",
		       sound.soft.stereo, sound.soft.stereo ? "stereo" : "mono");
	switch (sound.mach.type) {
	}
	len += sprintf(buffer+len, "\tsq.block_size = %d sq.max_count = %d"
		       " sq.max_active = %d\n",
		       sq.block_size, sq.max_count, sq.max_active);
	len += sprintf(buffer+len, "\tsq.count = %d sq.rear_size = %d\n", sq.count,
		       sq.rear_size);
	len += sprintf(buffer+len, "\tsq.active = %d sq.syncing = %d\n",
		       sq.active, sq.syncing);
	state.len = len;
	return 0;
}


static int state_release(struct inode *inode, struct file *file)
{
	state.busy = 0;
	MOD_DEC_USE_COUNT;
	return 0;
}


static ssize_t state_read(struct file *file, char *buf, size_t count,
			  loff_t *ppos)
{
	int n = state.len - state.ptr;
	if (n > count)
		n = count;
	if (n <= 0)
		return 0;
	if (copy_to_user(buf, &state.buf[state.ptr], n))
		return -EFAULT;
	state.ptr += n;
	return n;
}


static struct file_operations state_fops =
{
	llseek:  sound_lseek,
	read:    state_read,
	open:    state_open,
	release: state_release
};


static void __init state_init(void)
{
#ifndef MODULE
	int state_unit;
#endif
	state_unit = register_sound_special(&state_fops, SND_DEV_STATUS);
	if (state_unit < 0)
		return;
	state.busy = 0;
}


/*** Common stuff ********************************************************/

static loff_t sound_lseek(struct file *file, long long offset, int orig)
{
	return -ESPIPE;
}


/*** Config & Setup **********************************************************/


static int __init vr41xx_init(void)
{
	int has_sound = 0;
	sound.mach = machVr41xx;
	has_sound = 1;

	if (!has_sound)
		return -ENODEV;

	/* Set up sound queue, /dev/audio and /dev/dsp. */

	/* Set default settings. */
	sq_init();

	/* Set up /dev/sndstat. */
	state_init();

	/* Set up /dev/mixer. */
	mixer_init();
	if (!sound.mach.irqinit()) {
		printk(KERN_ERR "DMA sound driver: Interrupt initialization failed\n");
		return -ENODEV;
	}
#ifdef MODULE
	irq_installed = 1;
#endif

	printk(KERN_INFO "DMA sound driver installed, using %d buffers of %dk.\n",
	       numBufs, bufSize);

	return 0;
}

module_init(vr41xx_init);

#ifdef MODULE

static void __exit vr41xx_cleanup(void)
{
	if (irq_installed) {
		sound_silence();
		sound.mach.irqcleanup();
	}

	sq_release_buffers();

	if (mixer_unit >= 0)
		unregister_sound_mixer(mixer_unit);
	if (state_unit >= 0)
		unregister_sound_special(state_unit);
	if (sq_unit >= 0)
		unregister_sound_dsp(sq_unit);
}

module_exit(vr41xx_cleanup);

#else /* !MODULE */

static int __init dmasound_setup(char *str)
{
	int ints[6];

	str = get_options(str, ARRAY_SIZE(ints), ints);

	/* check the bootstrap parameter for "dmasound=" */

	switch (ints[0]) {
	case 3:
		if ((ints[3] < 0) || (ints[3] > MAX_CATCH_RADIUS))
			printk("dmasound_setup: illegal catch radius, using default = %d\n", catchRadius);
		else
			catchRadius = ints[3];
		/* fall through */
	case 2:
		if (ints[1] < MIN_BUFFERS)
			printk("dmasound_setup: illegal number of buffers, using default = %d\n", numBufs);
		else
			numBufs = ints[1];
		if (ints[2] < MIN_BUFSIZE || ints[2] > MAX_BUFSIZE)
			printk("dmasound_setup: illegal buffer size, using default = %d\n", bufSize);
		else
			bufSize = ints[2];
		break;
	case 0:
		break;
	default:
		printk("dmasound_setup: illegal number of arguments\n");
		return 0;
	}
	return 1;
}

__setup("dmasound=", dmasound_setup);

#endif /* MODULE */

/* vim:set ts=4:set sw=4: */
