/*
 * QEMU Casio Cassiopeia BE-300 Machine Model
 *
 * Copyright (c) 2024 Gemini CLI / jroark
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "cpu.h"
#include "hw/mips/mips.h"
#include "hw/mips/cpudevs.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/mips/bios.h"
#include "exec/address-spaces.h"
#include "sysemu/sysemu.h"
#include "sysemu/reset.h"
#include "hw/sysbus.h"
#include "hw/char/serial.h"
#include "elf.h"
#include "hw/intc/vr4131_icu.h"
#include "hw/timer/vr4131_rtc.h"

#define TYPE_BE300_MACHINE MACHINE_TYPE_NAME("be300")

#define BE300_RAM_SIZE        (16 * 1024 * 1024)
#define BE300_ROM_BASE        0x1fc00000
#define BE300_ROM_SIZE        (4 * 1024 * 1024)
#define BE300_VRIP_ICU_ADDR   0x0f000080
#define BE300_VRIP_RTC_ADDR   0x0f000100
#define BE300_UART_BASE       0x0a008680
#define BE300_UART_IRQ        19
#define BE300_UART_CLOCK      18432000

/* CyaCE Shim Handoff */
#define BE300_CMDLINE_ADDR    0x00004000 
#define BE300_ARGV_ADDR       0x00004800
#define MAX_CMDLINE_LEN       1024

typedef struct {
    MIPSCPU *cpu;
    uint64_t kernel_entry;
    char *kernel_cmdline;
} BE300ResetData;

static void be300_reset(void *opaque)
{
    BE300ResetData *s = opaque;
    CPUMIPSState *env = &s->cpu->env;

    cpu_reset(CPU(s->cpu));

    if (s->kernel_entry != -1ULL) {
        if (s->kernel_cmdline) {
            size_t len = strlen(s->kernel_cmdline);
            uint32_t argv_0_ptr = tswap32(BE300_CMDLINE_ADDR);
            uint32_t null_ptr = 0;
            uint8_t zero = 0;

            cpu_physical_memory_write(BE300_CMDLINE_ADDR, s->kernel_cmdline, len);
            cpu_physical_memory_write(BE300_CMDLINE_ADDR + len, &zero, 1);
            cpu_physical_memory_write(BE300_ARGV_ADDR, &argv_0_ptr, 4);
            cpu_physical_memory_write(BE300_ARGV_ADDR + 4, &null_ptr, 4);

            env->active_tc.gpr[4] = 1;                     /* a0 */
            env->active_tc.gpr[5] = BE300_ARGV_ADDR;      /* a1 */
        } else {
            env->active_tc.gpr[4] = 0;
            env->active_tc.gpr[5] = 0;
        }

        env->active_tc.gpr[6] = 0;                         /* a2 */
        env->active_tc.PC = s->kernel_entry;
    }
}

static void be300_init(MachineState *machine)
{
    const char *kernel_filename = machine->kernel_filename;
    const char *kernel_cmdline = machine->kernel_cmdline;
    const char *initrd_filename = machine->initrd_filename;
    MemoryRegion *address_space_mem = get_system_memory();
    MemoryRegion *ram = g_new(MemoryRegion, 1);
    MemoryRegion *rom = g_new(MemoryRegion, 1);
    BE300ResetData *reset_info = g_new0(BE300ResetData, 1);
    DeviceState *icu_dev;
    CPUMIPSState *env;
    
    /* CPU Selection */
    const char *cpu_type = machine->cpu_type;
    if (!cpu_type) cpu_type = MIPS_CPU_TYPE_NAME("Vr4100");
    if (!object_class_by_name(cpu_type)) {
        warn_report("CPU type '%s' not found, falling back to '4Kc'", cpu_type);
        cpu_type = MIPS_CPU_TYPE_NAME("4Kc");
    }
    
    reset_info->cpu = MIPS_CPU(cpu_create(cpu_type));
    env = &reset_info->cpu->env;
    reset_info->kernel_entry = -1ULL;

    /* Init RAM & ROM */
    memory_region_init_ram(ram, NULL, "be300.ram", BE300_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(address_space_mem, 0, ram);
    memory_region_init_rom(rom, NULL, "be300.rom", BE300_ROM_SIZE, &error_fatal);
    memory_region_add_subregion(address_space_mem, BE300_ROM_BASE, rom);

    /* ICU, RTC, UART */
    icu_dev = sysbus_create_simple(TYPE_VR4131_ICU, BE300_VRIP_ICU_ADDR, env->irq[2]);
    sysbus_create_simple(TYPE_VR4131_RTC, BE300_VRIP_RTC_ADDR, qdev_get_gpio_in(icu_dev, 0));
    serial_mm_init(address_space_mem, BE300_UART_BASE, 2,
                   qdev_get_gpio_in(icu_dev, BE300_UART_IRQ),
                   BE300_UART_CLOCK, serial_hd(0), DEVICE_NATIVE_ENDIAN);

    if (kernel_filename) {
        uint64_t kernel_low, kernel_high;
        if (load_elf(kernel_filename, NULL, cpu_mips_kseg0_to_phys, NULL,
                     &reset_info->kernel_entry, &kernel_low, &kernel_high, 
                     NULL, 0, EM_MIPS, 1, 0) < 0) {
            error_report("could not load kernel '%s'", kernel_filename);
            exit(1);
        }

        /* Initrd Loading */
        uint64_t initrd_base = 0;
        uint64_t initrd_size = 0;
        if (initrd_filename) {
            initrd_size = get_image_size(initrd_filename);
            if (initrd_size > 0) {
                if (initrd_size > BE300_RAM_SIZE) {
                    error_report("initrd '%s' is too large", initrd_filename);
                    exit(1);
                }
                /* Place at top of RAM, 64KB aligned */
                initrd_base = (BE300_RAM_SIZE - initrd_size) & ~0xFFFF;
                if (initrd_base < kernel_high) {
                    error_report("initrd too large, overlaps kernel");
                    exit(1);
                }
                if (load_image_targphys(initrd_filename, initrd_base, initrd_size) < 0) {
                    error_report("could not load initrd '%s'", initrd_filename);
                    exit(1);
                }
            }
        }

        /* Command Line Preparation */
        GString *cmdline = g_string_new(kernel_cmdline);
        if (initrd_size > 0) {
            /* Pass initrd location to kernel via command line tags */
            g_string_append_printf(cmdline, " rd_start=0x%" PRIx64 " rd_size=0x%" PRIx64,
                                   (uint64_t)0x80000000 + initrd_base, initrd_size);
        }

        if (cmdline->len > 0) {
            if (cmdline->len >= MAX_CMDLINE_LEN) {
                warn_report("Kernel command line truncated");
                g_string_truncate(cmdline, MAX_CMDLINE_LEN - 1);
            }
            reset_info->kernel_cmdline = g_string_free(cmdline, FALSE);
        } else {
            g_string_free(cmdline, TRUE);
        }
    }

    qemu_register_reset(be300_reset, reset_info);
}

static void be300_machine_init(MachineClass *mc)
{
    mc->desc = "Casio Cassiopeia BE-300 (VR4131)";
    mc->init = be300_init;
    mc->block_default_type = IF_NONE; 
    mc->max_cpus = 1;
    mc->default_ram_size = BE300_RAM_SIZE;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("Vr4100");
}

DEFINE_MACHINE("be300", be300_machine_init)
