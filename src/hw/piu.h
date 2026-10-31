#pragma once

#include <stdbool.h>
#include <stdint.h>

/* VRC4173 PIU interrupt/status bits as exposed by the BE-300 mapping. */
#define PIU_PENDING_PENCHG      0x01u
#define PIU_PENDING_DATALOST    0x04u
#define PIU_PENDING_PAGE0       0x08u
#define PIU_PENDING_PAGE1       0x10u
#define PIU_PENDING_ADPORT      0x40u   /* A/D port scan done (PADADPINTR) */
#define PIU_PENDING_TOUCH_DATA  (PIU_PENDING_PAGE0 | PIU_PENDING_PAGE1)
#define PIU_PENDING_COORDINATE  (PIU_PENDING_TOUCH_DATA | PIU_PENDING_DATALOST)
#define PIU_PENDING_TOUCH_GROUP 0x5Cu
#define PIU_PENDING_ANY         (PIU_PENDING_PENCHG | PIU_PENDING_TOUCH_GROUP)

#define PIU_PADSTATE_DISABLE       0u
#define PIU_PADSTATE_STANDBY       1u
#define PIU_PADSTATE_WAIT_PEN      4u
#define PIU_PADSTATE_PEN_DATA      5u
#define PIU_PADSTATE_INTERVAL      6u

/* VRC4173 UM sections 9.3.3 and 9.3.4 reset values. */
#define PIU_RESET_SCAN_INTERVAL 0x00A7u
#define PIU_RESET_STABLE        0x0007u

typedef struct {
    uint16_t regs[16];
    uint16_t padstate;
    bool touch_down;
    uint16_t touch_x;
    uint16_t touch_y;
    bool penstc;
    bool queued_penchg;
    bool queued_penstc;
    bool data_armed;
    uint64_t data_ready_ns;
    uint8_t next_page;
    uint16_t adc[4];
    uint16_t page_adc[2][4];
    bool page_valid[2];
    /*
     * The A/D side's enable, AA0003C8 bits 0-1 (both set), as it stood at
     * the last PIUCNT write. Windows CE leaves 3 there and a power-on 0; on
     * a real BE-300 the PIU never detected the pen or sampled until it was
     * 3, and setting it on a running sequencer took effect only once
     * PIUCNT was written again (glaze-os docs/NAND_BOOT_AUDIT.md,
     * "Power-on findings"). Without it the sequencer's states still change.
     */
    bool adc_on;
    /*
     * The A/D port scan Windows CE's battery driver runs (gwes.exe
     * 0xB0428-0xB0570): PIUMODE 01 (PIUCNT bit 3) with PIUSEQEN converts
     * the auxiliary input, raises PADADPINTR (PIUINT bit 6) and leaves the
     * 12-bit result at companion 0x0390, PIU offset 0x90. aux_adc is the
     * input's level, set by the machine (hw/battery.h) before each access.
     */
    uint16_t aux_adc;
    uint16_t adport;
} piu_state_t;

#define PIU_ADPORT_OFF      0x90u     /* companion 0x0390 */

#define PIU_ADC_ENABLE_OFF  0x03C8u   /* companion offset of the enable */

void piu_init(piu_state_t *p);
void piu_set_touch(piu_state_t *p, bool down, uint16_t x, uint16_t y,
    uint64_t now_ns);
void piu_tick(piu_state_t *p, uint64_t now_ns);
void piu_write(piu_state_t *p, uint32_t offset, uint16_t value,
    uint64_t now_ns);
uint16_t piu_read(piu_state_t *p, uint32_t offset, uint64_t now_ns);
bool piu_irq_pending(const piu_state_t *p);
