#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* VRC4173 NAND controller register offsets (from PA_VRC4173_BASE = 0x0A000000) */
#define NAND_CTRL_BASE    0xA000u   /* Legacy control/timing init registers */
#define NAND_CTRL_END     0xA020u
#define NAND_XFER_BASE    0xA400u   /* SPL transfer engine control/status */
#define NAND_XFER_END     0xA500u
#define NAND_BOOT_BASE    0xC000u   /* ROM-era transfer engine control/status */
#define NAND_BOOT_END     0xC100u
#define NAND_RESTORE_BUF_BASE  0x0B00u   /* NANDWRITER page buffer window */
#define NAND_RESTORE_BUF_END   0x0D00u
#define NAND_RESTORE_BASE      0x0C00u
#define NAND_RESTORE_SIDE_BASE 0x0980u   /* NANDWRITER sideband control */
#define NAND_RESTORE_SIDE_END  0x0990u
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
#define NAND_REG_BUFFER_BASE   0xA4A0u   /* mode-4/ECC result buffer */
#define NAND_REG_BUFFER_END    0xA4B0u
#define NAND_REG_XFER_STATUS2  0xA4C0u   /* ECC/result status latch */
#define NAND_REG_STREAM_DATA   0xB000u

/* ROM transfer-engine registers */
#define NAND_REG_BOOT_CTRL         0xC010u
#define NAND_REG_BOOT_CMD          0xC014u
#define NAND_REG_BOOT_ADDR         0xC020u
#define NAND_REG_BOOT_ACK          0xC030u
#define NAND_REG_BOOT_STATUS       0xC040u
#define NAND_REG_BOOT_KICK         0xC060u
#define NAND_REG_BOOT_MODE         0xC064u
#define NAND_REG_BOOT_ECC_IN       0xC068u
#define NAND_REG_BOOT_ECC_OUT_BASE 0xC0A0u
#define NAND_REG_BOOT_ECC_OUT_END  0xC0B0u
#define NAND_REG_BOOT_STATUS2      0xC0C0u

/* NANDWRITER restore-engine registers in the VRC4173 C-window */
#define NAND_REG_RESTORE_C00      0x0C00u
#define NAND_REG_RESTORE_C04      0x0C04u
#define NAND_REG_RESTORE_C08      0x0C08u
#define NAND_REG_RESTORE_C0C      0x0C0Cu
#define NAND_REG_RESTORE_C10      0x0C10u
#define NAND_REG_RESTORE_C14      0x0C14u
#define NAND_REG_RESTORE_C20      0x0C20u
#define NAND_REG_RESTORE_C24      0x0C24u
#define NAND_REG_RESTORE_C2C      0x0C2Cu
#define NAND_REG_RESTORE_C30      0x0C30u
#define NAND_REG_RESTORE_C34      0x0C34u
#define NAND_REG_RESTORE_C38      0x0C38u
#define NAND_REG_RESTORE_C3C      0x0C3Cu
#define NAND_REG_RESTORE_C40      0x0C40u
#define NAND_REG_RESTORE_C48      0x0C48u
#define NAND_REG_RESTORE_C4C      0x0C4Cu
#define NAND_REG_RESTORE_C60      0x0C60u
#define NAND_REG_RESTORE_C64      0x0C64u
#define NAND_REG_RESTORE_C68      0x0C68u
#define NAND_REG_RESTORE_CA0      0x0CA0u
#define NAND_REG_RESTORE_CAC      0x0CACu
#define NAND_REG_RESTORE_CC0      0x0CC0u

/* Individual legacy data-port registers */
#define NAND_REG_PORTCTL  0xD7F8u   /* Port control / command latch */
#define NAND_REG_DEVID    0xD7FAu   /* Device ID probe register */
#define NAND_REG_DATA     0xD7FCu   /* Data R/W (16-bit) */
#define NAND_REG_STATUS   0xD7FEu   /* Status */

/* Command/status registers */
#define NAND_REG_NFSTAT   0xA008u   /* NAND Flash Status (read-only, ready bits) */
#define NAND_REG_CMD      0xAC00u   /* Command opcode */
#define NAND_REG_ADDR     0xAC04u   /* Address byte */
#define NAND_REG_READY    0xAC48u   /* Ready/status (bit 0) */
#define NAND_DMA_BASE     0xC170u   /* DMA command/status/FIFO base */
#define NAND_DMA_END      0xC178u   /* End of DMA command block */
#define NAND_DMA_CTRL     0xC376u   /* DMA control/acknowledge */
#define NAND_REG_DIO_DATA 0xD200u   /* NAND Direct I/O data port */
#define NAND_REG_DIO_CTRL 0xD202u   /* NAND Direct I/O control (CLE/ALE) */

/* NAND geometry: 512B pages + 16B OOB = 528B per raw page */
#define NAND_PAGE_DATA    512u
#define NAND_PAGE_OOB     16u
#define NAND_PAGE_RAW     (NAND_PAGE_DATA + NAND_PAGE_OOB)
#define NAND_BLOCK_PAGES  32u
#define NAND_BLOCK_COUNT  1024u
#define NAND_IMAGE_SIZE   (NAND_BLOCK_COUNT * NAND_BLOCK_PAGES * NAND_PAGE_DATA)

/* NAND commands */
#define NAND_CMD_READ0    0x00u   /* Read area A (column 0-255) */
#define NAND_CMD_READ1    0x01u   /* Read area B (column 256-511) */
#define NAND_CMD_READOOB  0x50u   /* Read OOB area */
#define NAND_CMD_STATUS   0x70u   /* Read status */
#define NAND_CMD_SEQIN    0x80u   /* Program setup */
#define NAND_CMD_READID   0x90u   /* Read device ID */
#define NAND_CMD_PAGEPROG 0x10u   /* Program confirm */
#define NAND_CMD_ERASE1   0x60u   /* Erase setup */
#define NAND_CMD_ERASE2   0xD0u   /* Erase confirm */
#define NAND_CMD_RESET    0xFFu   /* Reset */

typedef enum {
    NAND_STATE_IDLE,
    NAND_STATE_READ_DATA,
    NAND_STATE_READ_OOB,
    NAND_STATE_READ_ID,
    NAND_STATE_READ_STATUS,
    NAND_STATE_PROGRAM_DATA,
    NAND_STATE_ERASE_SETUP,
} nand_cmd_state_t;

typedef struct {
    uint8_t       *image;       /* pointer to NAND image data (NULL = no image) */
    size_t         image_size;
    bool           dirty;
    int8_t         block_data_state[NAND_BLOCK_COUNT]; /* -1 unknown, 0 blank, 1 contains data */

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
    uint8_t  strap_regs[0x100]; /* 0xA060-0xA15F survey-backed latch */
    uint32_t xfer_regs[64];     /* 0xA400-0xA4FC */

    /* SPL transfer engine state (A420/A440/A460/A464/B000 path) */
    uint8_t  xfer_addr_bytes[4];
    uint8_t  xfer_addr_count;
    uint32_t xfer_addr24;
    uint32_t stream_page;
    uint32_t stream_col;
    uint32_t stream_base;
    uint32_t stream_cursor;
    uint32_t stream_limit;
    bool     stream_active;
    uint32_t boot_regs[64];      /* 0xC000-0xC0FC */
    uint8_t  boot_addr_bytes[4];
    uint8_t  boot_addr_count;
    uint16_t boot_ecc_words[6];
    uint8_t  boot_ecc_count;
    uint8_t  boot_mode;
    bool     boot_ready;
    uint16_t portctl;
    uint8_t  legacy_regs[256];
    bool     legacy_dirty[256];             /* per-index: write since last full ack */
    uint16_t legacy_read_since_write[256]; /* per-index: read count since last write */
    uint16_t legacy_status7_event_reads;   /* sustained idx7 event-bit poll counter */
    bool     legacy_status7_ff_armed;      /* one-shot 0xFF escape has been emitted */
    uint8_t  xfer_buffer[16];              /* mode-4/ECC result buffer (0xA4A0-0xA4AC) */
    uint8_t  xfer_ecc_count;               /* six 10-bit ECC inputs via 0xA468 */
    uint8_t  xfer_phase;                   /* legacy A414 phase latch */
    uint8_t  xfer_pending_byte;            /* pending command/address data byte */
    uint8_t  xfer_last_cmd;                /* committed A400 command byte */
    uint8_t  xfer_data_bytes[8];           /* queued A420 readback bytes */
    uint8_t  xfer_data_count;              /* queued A420 byte count */
    uint8_t  xfer_data_pos;                /* queued A420 byte cursor */
    uint8_t  dio_mode;                     /* D002 control: 0x80=CLE, 0x01=ALE, 0=data */
    uint8_t  dio_last_write;              /* last byte written to D000 (for echo-back) */

    /* DMA transfer engine state (0xC170-0xC377 path used by ROM) */
    uint8_t  dma_cmd[8];                  /* command block at 0xC170-0xC177 */
    uint32_t dma_nand_addr;               /* decoded NAND byte offset (page from cmd[3..6] × 512) */
    uint32_t dma_page_count;              /* pages to transfer (cmd byte 2) */
    uint32_t dma_cursor;                  /* byte cursor into current FIFO transfer */
    uint32_t dma_total_bytes;             /* total bytes available for FIFO reads */
    bool     dma_active;                  /* true when data is available for FIFO reads */
    uint8_t  status_value;
    uint8_t  program_buffer[NAND_PAGE_RAW];
    uint32_t program_column;
    uint32_t program_length;

    /* NANDWRITER restore-engine state (0x0A000B00 / 0x0A000C00 window) */
    uint32_t restore_regs[0x800];
    uint8_t  restore_page_buffer[NAND_PAGE_RAW];
    uint32_t restore_page_addr;
    uint32_t restore_stream_pos;
    uint32_t restore_stream_limit;
    uint32_t restore_stream_base;
    uint8_t  restore_phase;
    uint8_t  restore_mode;
    uint8_t  restore_last_cmd;
    uint8_t  restore_addr_bytes[4];
    uint8_t  restore_addr_count;
    uint16_t restore_ecc_words[6];
    uint8_t  restore_ecc_count;
    bool     restore_status_ok;
} nand_state_t;

void     nand_init(nand_state_t *s, uint8_t *image, size_t size);
uint64_t nand_read(nand_state_t *s, uint32_t offset, unsigned size,
                   uint32_t pc);
void     nand_write(nand_state_t *s, uint32_t offset, unsigned size,
                    uint64_t value, uint32_t pc);
bool     nand_restore_handles_offset(uint32_t offset);
uint64_t nand_restore_read(nand_state_t *s, uint32_t offset, unsigned size);
void     nand_restore_write(nand_state_t *s, uint32_t offset, unsigned size,
                            uint64_t value);
