#include <linux/init.h>
#include <linux/console.h>
#include <linux/types.h>

#define  	VIDEO_MEMORY   		0xAA200000
#define  	MARK_HEIGHT		4
#define  	MARK_WIDTH		4

/* base addr of uart and clock timing */
#define         BASE                    0xAA008680
#define         MAX_BAUD                1152000 /*9600*/
#define         REG_OFFSET              4

/* register offset */
#define         OFS_RCV_BUFFER          (0)
#define         OFS_TRANS_HOLD          (0)
#define         OFS_SEND_BUFFER         (0)
#define         OFS_INTR_ENABLE         (4)
#define         OFS_INTR_ID             (8)
#define         OFS_DATA_FORMAT         (3*REG_OFFSET)
#define         OFS_LINE_CONTROL        (3*REG_OFFSET)
#define         OFS_MODEM_CONTROL       (0x10)
#define         OFS_RS232_OUTPUT        (0x10)
#define         OFS_LINE_STATUS         (0x14)
#define         OFS_MODEM_STATUS        (6*REG_OFFSET)
#define         OFS_RS232_INPUT         (6*REG_OFFSET)
#define         OFS_SCRATCH_PAD         (7*REG_OFFSET)

#define         OFS_DIVISOR_LSB         (0*REG_OFFSET)
#define         OFS_DIVISOR_MSB         (1*REG_OFFSET)

/* memory-mapped read/write of the port */
#define		SIU_READ(reg)    	(*((volatile uint8_t *)(BASE + reg)))
#define 	SIU_WRITE(reg, val)  	((*((volatile uint8_t *)(BASE + reg))) = val)

#define         SIU_BAUD_2400             2400
#define         SIU_BAUD_4800             4800
#define         SIU_BAUD_9600             9600
#define         SIU_BAUD_19200            19200
#define         SIU_BAUD_38400            38400
#define         SIU_BAUD_57600            57600
#define         SIU_BAUD_115200           115200

#define         SIU_PARITY_NONE           0
#define         SIU_PARITY_ODD            0x08
#define         SIU_PARITY_EVEN           0x18
#define         SIU_PARITY_MARK           0x28
#define         SIU_PARITY_SPACE          0x38

#define         SIU_DATA_5BIT             0x0
#define         SIU_DATA_6BIT             0x1
#define         SIU_DATA_7BIT             0x2
#define         SIU_DATA_8BIT             0x3

#define         SIU_STOP_1BIT             0x0
#define         SIU_STOP_2BIT             0x4

void debug_mark(int x, int y, uint16_t color) 
{
    static uint16_t * screen = (uint16_t *) VIDEO_MEMORY;
    int xx, yy;
    
    for (yy = 0; yy < MARK_HEIGHT; ++yy) {
	for (xx = 0; xx < MARK_WIDTH; ++xx) {
	    screen[(x * MARK_WIDTH  + xx) +
		   (y * MARK_HEIGHT + yy) * 256] = color;
        }
    }
}

static void be300_console_outchar(char c) 
{
    while ((SIU_READ(OFS_LINE_STATUS) & 0x20) == 0);
    SIU_WRITE(OFS_SEND_BUFFER, c);
}

static void be300_console_write(struct console * co, 
				const char * s, unsigned count)
{
    unsigned i;

    for (i = 0; i < count; i++) {
        if (s[i] == '\n') be300_console_outchar('\r');
        be300_console_outchar(s[i]);
    }
}

static int __init be300_console_setup(struct console * co, char * options)
{
    static char message[] = "Debug serial output is initialized\n";

    uint32_t baud 	= SIU_BAUD_9600;
    uint8_t  data 	= SIU_DATA_8BIT;
    uint8_t  parity	= SIU_PARITY_NONE;
    uint8_t  stop	= SIU_STOP_1BIT;
    
    uint32_t divisor;
				
    /* disable interrupts */
    SIU_WRITE(OFS_LINE_CONTROL, 0x0);
    SIU_WRITE(OFS_INTR_ENABLE, 0);
		
    /* set up buad rate */
				       
    /* set DIAB bit */
    SIU_WRITE(OFS_LINE_CONTROL, 0x80);
						            
    /* set divisor */
    divisor = MAX_BAUD / baud;
    SIU_WRITE(OFS_DIVISOR_LSB, divisor & 0xff);
    SIU_WRITE(OFS_DIVISOR_MSB, (divisor & 0xff00) >> 8);

    /* clear DIAB bit */
    SIU_WRITE(OFS_LINE_CONTROL, 0x0);
    
    /* set data format */
    SIU_WRITE(OFS_DATA_FORMAT, data | parity | stop);
							
    be300_console_write(co, message, sizeof(message) - 1);

    return 0;
}

static struct console be300_cons = {
    .name           = "be300",
    .write          = be300_console_write,
    .setup          = be300_console_setup,
    .flags          = CON_PRINTBUFFER,
    .index          = -1,
};
					
/*
 *    Register console.
 */
/*static*/ int /*__init*/ be300_console_init(void)
{
    debug_mark(0, 0, 0xF800);

    register_console(&be300_cons);
    return 0;
}
/*console_initcall(be300_console_init);*/
