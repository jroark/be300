/*
 * QEMU NEC VR4131 RTC (Real Time Clock)
 *
 * Copyright (c) 2024 Gemini CLI / jroark
 */

#ifndef HW_TIMER_VR4131_RTC_H
#define HW_TIMER_VR4131_RTC_H

#include "hw/sysbus.h"
#include "qemu/timer.h"

#define TYPE_VR4131_RTC "vr4131-rtc"
#define VR4131_RTC(obj) OBJECT_CHECK(VR4131RTCState, (obj), TYPE_VR4131_RTC)

typedef struct VR4131RTCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *timer;

    /* Registers (Simplified) */
    uint16_t etimel;
    uint16_t etimem;
    uint16_t etimeh;
    uint16_t rtc_long1;
    uint16_t rtc_int_stat; /* Status bits */
} VR4131RTCState;

#endif /* HW_TIMER_VR4131_RTC_H */
