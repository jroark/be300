#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * ScCmcu: the companion's window onto Casio's power-management
 * microcontroller (PA 0x0A007800-0x0A00783F, docs/hardware/hardware.txt:189).
 * Its only known function is a watchdog that resets the BE-300 unless it is
 * reloaded or stopped. Nothing documents the protocol; this follows what
 * the boot ROM and Windows CE send:
 *
 *   stop   ARG0 (+0x04) = 0x35, then CMD0 (+0x00) = 0x5C, resent while
 *          STATUS (+0x34) reads 3: the boot ROM at 0x9FC00D20-0x9FC00D68
 *          (MIPS16), and CE's StopWDT (cedmbltin.dll 0x019F1A6C), which
 *          polls until STATUS & 7 reads 0.
 *   start  TIMEOUT (+0x14) = seconds, then ARG (+0x0C) = 0x53 and CMD
 *          (+0x08) = 0x3A, resent until STATUS & 7 reads 3 (CE's start loop,
 *          cedmbltin.dll 0x019F1990).
 *   reload TIMEOUT again (CE every ~9 s; Glaze's kernel/drivers/cmcu.c
 *          every 5 s).
 *
 * So STATUS reads 3 while the watchdog runs and 0 once stopped. A real
 * BE-300 read 3 after Windows CE; it reset 140-150 s after CE stopped
 * reloading TIMEOUT = 0x96 (150), and did not across Glaze's 155 s and
 * 163 s sleeps once Glaze sent the stop (glaze-os kernel/drivers/cmcu.c,
 * docs/POWER.md, 2026-10-31).
 *
 * CONFIG (+0x10) bit 16 reports that the watchdog reset the system. CE's
 * driver reads it when it loads (cedmbltin.dll 0x019F1FE8-0x019F2028): if
 * the bit is set it prints "System was rebooted by WDT." and sets bit 3 of
 * the boot flags at PA 0x254C, which PowerOn.dll (0x01A1140C-0x01A11524)
 * turns into the "Restart: Due to an OS problem" message; then it writes
 * 0x10000 to CONFIG whatever it read. So the bit is set by an expiry and
 * cleared by writing 1 to it. As a read-back latch it stayed set after the
 * driver's first write, and every later boot reported a watchdog reset.
 * The other registers read back as written.
 */
#define CMCU_CMD0     0x00u
#define CMCU_ARG0     0x04u
#define CMCU_CMD      0x08u
#define CMCU_ARG      0x0Cu
#define CMCU_CONFIG   0x10u
#define CMCU_TIMEOUT  0x14u
#define CMCU_STATUS   0x34u
#define CMCU_SIZE     0x40u

#define CMCU_STATUS_RUNNING 3u
#define CMCU_CONFIG_WDT_RESET 0x00010000u  /* the watchdog reset the system; W1C */

typedef struct {
    uint32_t regs[CMCU_SIZE / 4];
    bool     running;
    uint64_t reloaded_ns;   /* emulated time of the last reload */
} cmcu_state_t;

void     cmcu_init(cmcu_state_t *s);
uint32_t cmcu_read(const cmcu_state_t *s, uint32_t off);
void     cmcu_write(cmcu_state_t *s, uint32_t off, uint32_t val, uint64_t now_ns);
/* True once when a running watchdog's TIMEOUT has passed unreloaded; it
 * stops then, as the reset it causes leaves it. */
bool     cmcu_expired(cmcu_state_t *s, uint64_t now_ns);
