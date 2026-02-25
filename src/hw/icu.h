#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * ICU — Interrupt Control Unit (VR4131 hardware manual §11)
 *
 * The ICU aggregates interrupt sources from all peripherals into
 * the MIPS IP (Interrupt Pending) bits of CP0 Cause.  Two system
 * interrupt registers (SYSINT1/2) hold pending flags; corresponding
 * mask registers (MSYSINT1/2) gate delivery.
 *
 * Interrupt sources are 16-bit flags. A source is deliverable when its
 * bit is set in SYSINTx AND set in MSYSINTx (active-high mask/enable).
 */

/* Register offsets from ICU base (0x0F000080) */
#define ICU_SYSINT1REG   0x00u   /* System interrupt 1 status  */
#define ICU_PIUINTREG    0x02u   /* PIU interrupt status        */
#define ICU_ATCINT       0x04u   /* ATC interrupt status        */
#define ICU_KIUINTREG    0x06u   /* KIU interrupt status        */
#define ICU_GIUINTLREG   0x08u   /* GIU interrupt low status    */
#define ICU_DSIUINTREG   0x0Au   /* DSIU interrupt status       */
#define ICU_MSYSINT1REG  0x0Cu   /* System interrupt 1 mask     */
#define ICU_MPIUINTREG   0x0Eu   /* PIU interrupt mask          */
#define ICU_MATCINT      0x10u   /* ATC interrupt mask          */
#define ICU_MKIUINTREG   0x12u   /* KIU interrupt mask          */
#define ICU_MGIUINTLREG  0x14u   /* GIU interrupt low mask      */
#define ICU_MDSIUINTREG  0x16u   /* DSIU interrupt mask         */
#define ICU_NMIREG       0x18u   /* NMI control                 */
#define ICU_SOFTINTREG   0x1Au   /* Software interrupt          */
#define ICU_SYSINT2REG   0x20u   /* System interrupt 2 status   */
#define ICU_GIUINTHREG   0x22u   /* GIU interrupt high status   */
#define ICU_FIRINTREG    0x24u   /* FIR interrupt status        */
#define ICU_MSYSINT2REG  0x2Cu   /* System interrupt 2 mask     */
#define ICU_MGIUINTHREG  0x2Eu   /* GIU interrupt high mask     */
#define ICU_MFIRINTREG   0x30u   /* FIR interrupt mask          */

/* SYSINT1 source bits */
#define ICU_SRC1_POWER   (1u << 0)
#define ICU_SRC1_RTC     (1u << 2)   /* RTCLONG1 */
#define ICU_SRC1_ETIME   (1u << 3)   /* ELAPSEDTIME */
#define ICU_SRC1_PIU     (1u << 5)
#define ICU_SRC1_AIU     (1u << 6)
#define ICU_SRC1_KIU     (1u << 7)
#define ICU_SRC1_GIU     (1u << 8)
#define ICU_SRC1_SIU     (1u << 9)   /* SIU */
#define ICU_SRC1_WDOG    (1u << 10)
#define ICU_SRC1_SOFT    (1u << 11)

typedef struct {
    uint16_t sysint1;    /* pending sources (set by peripherals) */
    uint16_t msysint1;   /* mask/enable: 1 = enabled             */
    uint16_t sysint2;
    uint16_t msysint2;
    uint16_t piuint,  mpiuint;
    uint16_t kiuint,  mkiuint;
    uint16_t giuintl, mgiuintl;
    uint16_t giuinth, mgiuinth;
    uint16_t dsiuint, mdsiuint;
    uint16_t firint,  mfirint;
    uint16_t softint;
    uint16_t nmi;
} icu_state_t;

void     icu_init    (icu_state_t *s);
uint32_t icu_read    (icu_state_t *s, uint32_t offset, unsigned size);
void     icu_write   (icu_state_t *s, uint32_t offset, unsigned size, uint32_t val);

/* Assert/deassert a SYSINT1 source bit (called by peripheral drivers) */
void     icu_assert  (icu_state_t *s, uint16_t src_bit);
void     icu_deassert(icu_state_t *s, uint16_t src_bit);

/* Returns true if any unmasked interrupt is pending */
bool     icu_pending (const icu_state_t *s);
