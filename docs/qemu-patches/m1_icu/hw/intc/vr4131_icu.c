/*
 * QEMU NEC VR4131 ICU (Interrupt Control Unit)
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/intc/vr4131_icu.h"

/* Register Offsets */
#define REG_SYSINT1   0x00
#define REG_SYSINT2   0x02
#define REG_GIUINTL   0x04
#define REG_GIUINTH   0x06
#define REG_MSYSINT1  0x08
#define REG_MSYSINT2  0x0a
#define REG_MGIUINTL  0x0c
#define REG_MGIUINTH  0x0e
#define REG_NMI       0x10
#define REG_SOFTINT   0x12
#define REG_INTKIND   0x14

static void vr4131_icu_update(VR4131ICUState *s)
{
    uint16_t pending1 = s->sysint1 & s->msysint1;
    uint16_t pending2 = s->sysint2 & s->msysint2;
    uint16_t pending_giu_l = s->giuintl & s->mgiuintl;
    uint16_t pending_giu_h = s->giuinth & s->mgiuinth;

    if (pending1 || pending2 || pending_giu_l || pending_giu_h || (s->softint & 0x1)) {
        qemu_set_irq(s->parent_irq, 1);
    } else {
        qemu_set_irq(s->parent_irq, 0);
    }
}

/* GPIO Input Handler: Updates Status Registers based on external IRQ lines */
static void vr4131_icu_set_irq(void *opaque, int irq, int level)
{
    VR4131ICUState *s = opaque;

    /* 
     * Simplified Mapping:
     * IRQ 0-15  -> SYSINT1
     * IRQ 16-31 -> SYSINT2
     */
    if (irq < 16) {
        if (level) {
            s->sysint1 |= (1 << irq);
        }
        /* Status bits usually stay set until W1C by guest */
    } else if (irq < 32) {
        if (level) {
            s->sysint2 |= (1 << (irq - 16));
        }
    }

    vr4131_icu_update(s);
}

static uint64_t vr4131_icu_read(void *opaque, hwaddr addr, unsigned size)
{
    VR4131ICUState *s = opaque;
    if (size != 2) return 0;

    switch (addr) {
    case REG_SYSINT1:   return s->sysint1;
    case REG_SYSINT2:   return s->sysint2;
    case REG_GIUINTL:   return s->giuintl;
    case REG_GIUINTH:   return s->giuinth;
    case REG_MSYSINT1:  return s->msysint1;
    case REG_MSYSINT2:  return s->msysint2;
    case REG_MGIUINTL:  return s->mgiuintl;
    case REG_MGIUINTH:  return s->mgiuinth;
    case REG_NMI:       return s->nmi;
    case REG_SOFTINT:   return s->softint;
    case REG_INTKIND:   return s->intkind;
    default:            return 0;
    }
}

static void vr4131_icu_write(void *opaque, hwaddr addr, uint64_t val64, unsigned size)
{
    VR4131ICUState *s = opaque;
    uint16_t val = (uint16_t)val64;
    if (size != 2) return;

    switch (addr) {
    case REG_SYSINT1:   s->sysint1 &= ~val; break; /* W1C */
    case REG_SYSINT2:   s->sysint2 &= ~val; break;
    case REG_GIUINTL:   s->giuintl &= ~val; break;
    case REG_GIUINTH:   s->giuinth &= ~val; break;
    case REG_MSYSINT1:  s->msysint1 = val; break;
    case REG_MSYSINT2:  s->msysint2 = val; break;
    case REG_MGIUINTL:  s->mgiuintl = val; break;
    case REG_MGIUINTH:  s->mgiuinth = val; break;
    case REG_SOFTINT:
        if (val & 0x2) s->softint &= ~0x1;
        else if (val & 0x1) s->softint |= 0x1;
        break;
    case REG_INTKIND:   s->intkind = val; break;
    }
    vr4131_icu_update(s);
}

static const MemoryRegionOps vr4131_icu_ops = {
    .read = vr4131_icu_read,
    .write = vr4131_icu_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = { .min_access_size = 2, .max_access_size = 2 },
};

static void vr4131_icu_init(Object *obj)
{
    VR4131ICUState *s = (VR4131ICUState *)obj;
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &vr4131_icu_ops, s, "vr4131-icu", 0x20);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->parent_irq);

    /* Initialize 32 GPIO input lines for external devices */
    qdev_init_gpio_in(DEVICE(obj), vr4131_icu_set_irq, 32);
}

static void vr4131_icu_reset(DeviceState *dev)
{
    VR4131ICUState *s = (VR4131ICUState *)dev;
    s->sysint1 = 0; s->sysint2 = 0; s->giuintl = 0; s->giuinth = 0;
    s->msysint1 = 0; s->msysint2 = 0; s->mgiuintl = 0; s->mgiuinth = 0;
    s->nmi = 0; s->softint = 0; s->intkind = 0;
    vr4131_icu_update(s);
}

static void vr4131_icu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->reset = vr4131_icu_reset;
}

static const TypeInfo vr4131_icu_info = {
    .name          = TYPE_VR4131_ICU,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VR4131ICUState),
    .instance_init = vr4131_icu_init,
    .class_init    = vr4131_icu_class_init,
};

static void vr4131_icu_register_types(void) { type_register_static(&vr4131_icu_info); }
type_init(vr4131_icu_register_types)
