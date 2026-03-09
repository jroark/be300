#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* VRC4173 NAND controller register offsets (from PA_VRC4173_BASE = 0x0A000000) */
#define NAND_CTRL_BASE    0xA000u   /* Legacy control/timing init registers */
#define NAND_CTRL_END     0xA020u
#define NAND_XFER_BASE    0xA400u   /* SPL transfer engine control/status */
#define NAND_XFER_END     0xA500u
#define NAND_CMD_BASE     0xAC00u   /* Legacy command/address registers */
#define NAND_CMD_END      0xAC50u
#define NAND_ENABLE_BASE  0xB100u   /* Legacy enable registers */
#define NAND_ENABLE_END   0xB110u
#define NAND_STREAM_BASE  0xB000u   /* SPL data stream window */
#define NAND_STREAM_END   0xB210u
#define NAND_DATA_BASE    0xD7F8u   /* Legacy data port registers */
#define NAND_DATA_END     0xD800u

/* SPL transfer-engine registers */
#define NAND_REG_XFER_CTRL     0xA410u
#define NAND_REG_XFER_CMD      0xA414u
#define NAND_REG_XFER_ADDR     0xA420u
#define NAND_REG_XFER_ACK      0xA430u
#define NAND_REG_XFER_STATUS   0xA440u
#define NAND_REG_XFER_KICK     0xA460u
#define NAND_REG_XFER_MODE     0xA464u
#define NAND_REG_XFER_MISC     0xA468u
#define NAND_REG_STREAM_DATA   0xB000u

/* Individual legacy data-port registers */
#define NAND_REG_PORTCTL  0xD7F8u   /* Port control / command latch */
#define NAND_REG_DEVID    0xD7FAu   /* Device ID probe register */
#define NAND_REG_DATA     0xD7FCu   /* Data R/W (16-bit) */
#define NAND_REG_STATUS   0xD7FEu   /* Status */

/* Command/status registers */
#define NAND_REG_CMD      0xAC00u   /* Command opcode */
#define NAND_REG_ADDR     0xAC04u   /* Address byte */
#define NAND_REG_READY    0xAC48u   /* Ready/status (bit 0) */

/* NAND geometry: 512B pages + 16B OOB = 528B per raw page */
#define NAND_PAGE_DATA    512u
#define NAND_PAGE_OOB     16u
#define NAND_PAGE_RAW     (NAND_PAGE_DATA + NAND_PAGE_OOB)

/* NAND commands */
#define NAND_CMD_READ0    0x00u   /* Read area A (column 0-255) */
#define NAND_CMD_READ1    0x01u   /* Read area B (column 256-511) */
#define NAND_CMD_READOOB  0x50u   /* Read OOB area */
#define NAND_CMD_READID   0x90u   /* Read device ID */
#define NAND_CMD_RESET    0xFFu   /* Reset */

typedef enum {
    NAND_STATE_IDLE,
    NAND_STATE_READ_DATA,
    NAND_STATE_READ_OOB,
    NAND_STATE_READ_ID,
} nand_cmd_state_t;

typedef struct {
    const uint8_t *image;       /* pointer to NAND image data (NULL = no image) */
    size_t         image_size;

    /* State machine */
    nand_cmd_state_t state;
    uint32_t page_addr;         /* current page address */
    uint32_t column;            /* column offset within page */
    uint32_t nand_offset;       /* byte offset into image for current transfer */
    uint32_t xfer_length;       /* remaining bytes in current transfer */
    uint32_t xfer_cursor;       /* bytes already read in current transfer */
    uint8_t  addr_cycle;        /* address byte cycle counter (0-3) */
    uint8_t  last_cmd;          /* last command written */
    bool     ready;             /* controller ready flag */
    bool     enabled;           /* controller enabled */

    /* Latched control registers */
    uint32_t ctrl_regs[8];      /* 0xA000-0xA01C */
    uint32_t xfer_regs[64];     /* 0xA400-0xA4FC */

    /* SPL transfer engine state (A420/A440/A460/A464/B000 path) */
    uint8_t  xfer_addr_bytes[4];
    uint8_t  xfer_addr_count;
    uint32_t xfer_addr24;
    uint32_t stream_base;
    uint32_t stream_cursor;
    bool     stream_active;
    uint16_t portctl;
} nand_state_t;

void     nand_init(nand_state_t *s, const uint8_t *image, size_t size);
uint64_t nand_read(nand_state_t *s, uint32_t offset, unsigned size, bool log);
void     nand_write(nand_state_t *s, uint32_t offset, unsigned size,
                    uint64_t value, bool log);
