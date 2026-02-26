/*
 * QEMU NEC VR4131 ICU (Interrupt Control Unit)
 *
 * Copyright (c) 2024 Gemini CLI / jroark
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 */

#ifndef HW_INTC_VR4131_ICU_H
#define HW_INTC_VR4131_ICU_H

#include "hw/sysbus.h"

#define TYPE_VR4131_ICU "vr4131-icu"
#define VR4131_ICU(obj) OBJECT_CHECK(VR4131ICUState, (obj), TYPE_VR4131_ICU)

typedef struct VR4131ICUState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq parent_irq; /* Output to CPU */

    /* Registers */
    uint16_t sysint1;
    uint16_t sysint2;
    uint16_t giuintl;
    uint16_t giuinth;
    uint16_t msysint1;
    uint16_t msysint2;
    uint16_t mgiuintl;
    uint16_t mgiuinth;
    uint16_t nmi;
    uint16_t softint;
    uint16_t intkind;

} VR4131ICUState;

#endif /* HW_INTC_VR4131_ICU_H */
