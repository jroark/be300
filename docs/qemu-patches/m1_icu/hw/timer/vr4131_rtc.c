/*
 * QEMU NEC VR4131 RTC (Real Time Clock)
 */

#include "qemu/osdep.h"
#include "hw/timer/vr4131_rtc.h"
#include "hw/irq.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/module.h"

#define REG_ETIMEL     0x00
#define REG_ETIMEM     0x02
#define REG_ETIMEH     0x04
#define REG_RTCLONG1   0x10
#define REG_RTCINTSTAT 0x1e

static void vr4131_rtc_update_irq(VR4131RTCState *s)
{
    qemu_set_irq(s->irq, !!(s->rtc_int_stat & 0x1));
}

static void vr4131_rtc_timer_cb(void *opaque)
{
    VR4131RTCState *s = opaque;
    s->rtc_int_stat |= 0x1;
    vr4131_rtc_update_irq(s);
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / 100);
}

static uint64_t vr4131_rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    VR4131RTCState *s = opaque;
    if (size != 2) return 0;
    switch (addr) {
    case REG_ETIMEL:     return s->etimel++;
    case REG_ETIMEM:     return s->etimem;
    case REG_ETIMEH:     return s->etimeh;
    case REG_RTCLONG1:   return s->rtc_long1;
    case REG_RTCINTSTAT: return s->rtc_int_stat;
    default:             return 0;
    }
}

static void vr4131_rtc_write(void *opaque, hwaddr addr, uint64_t val64, unsigned size)
{
    VR4131RTCState *s = opaque;
    uint16_t val = (uint16_t)val64;
    if (size != 2) return;

    switch (addr) {
    case REG_RTCLONG1:
        s->rtc_long1 = val;
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / 100);
        break;
    case REG_RTCINTSTAT:
        if (val & 0x1) s->rtc_int_stat &= ~0x1;
        vr4131_rtc_update_irq(s);
        break;
    }
}

static const MemoryRegionOps vr4131_rtc_ops = {
    .read = vr4131_rtc_read, .write = vr4131_rtc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = { .min_access_size = 2, .max_access_size = 2 },
};

static void vr4131_rtc_init(Object *obj)
{
    VR4131RTCState *s = (VR4131RTCState *)obj;
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    memory_region_init_io(&s->iomem, obj, &vr4131_rtc_ops, s, "vr4131-rtc", 0x20);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, vr4131_rtc_timer_cb, s);
}

static void vr4131_rtc_reset(DeviceState *dev)
{
    VR4131RTCState *s = (VR4131RTCState *)dev;
    s->etimel = 0; s->etimem = 0; s->etimeh = 0;
    s->rtc_long1 = 0; s->rtc_int_stat = 0;
    timer_del(s->timer);
    vr4131_rtc_update_irq(s);
}

static void vr4131_rtc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->reset = vr4131_rtc_reset;
}

static const TypeInfo vr4131_rtc_info = {
    .name          = TYPE_VR4131_RTC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VR4131RTCState),
    .instance_init = vr4131_rtc_init,
    .class_init    = vr4131_rtc_class_init,
};

static void vr4131_rtc_register_types(void) { type_register_static(&vr4131_rtc_info); }
type_init(vr4131_rtc_register_types)
