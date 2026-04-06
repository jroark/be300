/*
 *  machine_be300.c — BE-300 machine setup and run loop for GXemul.
 *
 *  Replaces the old 7800-line Unicorn machine.c. GXemul handles CP0, TLB,
 *  exceptions, and address translation natively.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <inttypes.h>
#include <time.h>

#include "be300.h"
#include "host_io.h"
#include "loader.h"
#include "ui.h"
#include "wince_boot.h"

/* GXemul headers */
#include "interrupt.h"
#include "cpu.h"
#include "cpu_mips.h"
#include "cop0.h"
#include "emul.h"
#include "device.h"
#include "devices.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"
#include "console.h"
#include "timer.h"
#include "settings.h"

/* GXemul externs */
extern volatile bool emul_shutdown;
extern volatile bool emul_executing;

static void be300_handle_stop_signal(int signum)
{
    (void)signum;
    emul_shutdown = true;
    __sync_synchronize();
}

static void be300_serial_ring_push(machine_t *m, int ch)
{
    if (!m)
        return;

    if (m->serial_count == BE300_SERIAL_RING_CAP) {
        m->serial_tail = (m->serial_tail + 1u) % BE300_SERIAL_RING_CAP;
        m->serial_count--;
    }

    m->serial_ring[m->serial_head] = (char)ch;
    m->serial_head = (m->serial_head + 1u) % BE300_SERIAL_RING_CAP;
    m->serial_count++;
}

static void be300_serial_sink(int ch, void *user_data)
{
    be300_serial_ring_push((machine_t *)user_data, ch);
}

static void be300_register_linux_input_if_needed(machine_t *m)
{
    if (m->input_registered)
        return;

    extern void be300_register_input(struct machine *, machine_t *, bool);
    be300_register_input(m->gxe_machine, m, m->cfg.log_mmio);
    m->input_registered = true;
}

static void be300_set_linux_boot_strings(machine_t *m,
                                         const char *kernel_name,
                                         const char *cmdline)
{
    struct machine *gxm = m->gxe_machine;
    const char *safe_kernel = kernel_name ? kernel_name : "vmlinux";
    const char *safe_cmdline = cmdline ? cmdline : "";

    free(gxm->boot_kernel_filename);
    gxm->boot_kernel_filename = strdup(safe_kernel);

    free(gxm->boot_string_argument);
    gxm->boot_string_argument = strdup(safe_cmdline);

    gxm->bootstr = gxm->boot_kernel_filename;
    gxm->bootarg = gxm->boot_string_argument[0] ? gxm->boot_string_argument : NULL;
}

static void be300_refresh_linux_bootinfo(machine_t *m)
{
    struct machine *gxm = m->gxe_machine;
    uint64_t ram_top;
    uint64_t argv_base;

    if (!gxm->prom_emulation)
        return;

    ram_top = 0x80000000ULL + ((uint64_t)gxm->physical_ram_in_mb << 20);
    argv_base = ram_top - 512;

    m->cpu->cd.mips.gpr[MIPS_GPR_A0] = 1;
    m->cpu->cd.mips.gpr[MIPS_GPR_A1] = argv_base;
    m->cpu->cd.mips.gpr[MIPS_GPR_A2] = ram_top - 256;

    store_32bit_word(m->cpu, argv_base, argv_base + 16);
    store_32bit_word(m->cpu, argv_base + 4, 0);
    store_32bit_word(m->cpu, argv_base + 8, 0);
    store_string(m->cpu, argv_base + 16, gxm->boot_kernel_filename);

    if (gxm->boot_string_argument && gxm->boot_string_argument[0]) {
        m->cpu->cd.mips.gpr[MIPS_GPR_A0]++;
        store_32bit_word(m->cpu, argv_base + 4, argv_base + 64);
        store_32bit_word(m->cpu, argv_base + 8, 0);
        store_string(m->cpu, argv_base + 64, gxm->boot_string_argument);
    }
}

static int be300_boot_linux_path(machine_t *m, const char *kernel_path,
                                 const char *cmdline, const char *ram_path)
{
    uint32_t entry_va = 0;
    uint32_t jiffies_pa = 0;

    if (!kernel_path)
        return -1;

    be300_set_linux_boot_strings(m, kernel_path, cmdline);
    be300_refresh_linux_bootinfo(m);

    if (loader_load_elf(m, kernel_path, &entry_va, &jiffies_pa) != 0) {
        fprintf(stderr, "[BE300] Failed to load kernel ELF\n");
        return -1;
    }

    m->cpu->pc = (uint64_t)(int32_t)entry_va;
    m->boot_mode = BE300_BOOT_LINUX_PATH;
    fprintf(stderr, "[BE300] Kernel entry: PC=0x%08X\n", entry_va);

    if (cmdline && cmdline[0])
        fprintf(stderr, "[BE300] Kernel cmdline: %s\n", cmdline);

    if (ram_path)
        loader_load_ram(m, ram_path);

    be300_register_linux_input_if_needed(m);
    return 0;
}

static int be300_boot_linux_memory_internal(machine_t *m,
                                            const void *kernel_data,
                                            size_t kernel_size,
                                            const char *cmdline)
{
    uint32_t entry_va = 0;
    uint32_t jiffies_pa = 0;

    if (!m || !kernel_data || kernel_size == 0 || !cmdline) {
        fprintf(stderr, "[BE300] Web boot requires a kernel image\n");
        return -1;
    }
    if (m->boot_mode != BE300_BOOT_NONE) {
        fprintf(stderr, "[BE300] Machine already has boot media loaded\n");
        return -1;
    }

    be300_set_linux_boot_strings(m, "vmlinux", cmdline);
    be300_refresh_linux_bootinfo(m);

    if (loader_load_elf_from_memory(m, kernel_data, kernel_size,
                                    &entry_va, &jiffies_pa) != 0) {
        fprintf(stderr, "[BE300] Failed to load in-memory kernel ELF\n");
        return -1;
    }

    m->cpu->pc = (uint64_t)(int32_t)entry_va;
    m->boot_mode = BE300_BOOT_LINUX_MEMORY;
    fprintf(stderr, "[BE300] In-memory kernel entry: PC=0x%08X\n", entry_va);
    fprintf(stderr, "[BE300] Kernel cmdline: %s\n", cmdline);

    be300_register_linux_input_if_needed(m);
    return 0;
}


machine_t *be300_create(const machine_config_t *cfg)
{
    static bool subsystems_initialized = false;
    machine_t *m = calloc(1, sizeof(machine_t));
    if (!m) return NULL;
    m->cfg = *cfg;
    m->boot_mode = BE300_BOOT_NONE;
    m->use_builtin_ui = true;
    m->save_exit_screenshot = true;
    m->mirror_serial_to_stdout = true;
    m->fb_width = 240;
    m->fb_height = 320;
    m->fb_stride = 256;
    wince_boot_init(m);

    /*
     * Initialize GXemul subsystems once per process.
     */
    if (!subsystems_initialized) {
        debugmsg_init();

        /* Initialize global settings before console_init needs them */
        extern void be300_init_global_settings(void);
        be300_init_global_settings();

        cpu_init();
        device_init();
        timer_init();
        console_init();

        /* Initialize machine type registry (registers hpcmips) */
        machine_init();

        subsystems_initialized = true;
    }

    /* Reset global execution flags for a new run */
    emul_executing = false;
    emul_shutdown = false;

    /*
     * Create the GXemul emul and machine objects.
     */
    m->emul = emul_new("be300");
    if (!m->emul) {
        fprintf(stderr, "[BE300] emul_new failed\n");
        free(m);
        return NULL;
    }

    struct machine *gxm = emul_add_machine(m->emul, "be300");
    if (!gxm) {
        fprintf(stderr, "[BE300] emul_add_machine failed\n");
        free(m);
        return NULL;
    }
    m->gxe_machine = gxm;

    /*
     * Configure as BE-300.
     */
    gxm->machine_type = MACHINE_HPCMIPS;
    gxm->machine_subtype = MACHINE_HPCMIPS_CASIO_BE300;
    gxm->cpu_name = strdup("VR4131");
    gxm->physical_ram_in_mb = cfg->sdram_size / (1024 * 1024);
    /* Enable prom emulation for Linux (sets up hpc_bootinfo, argc/argv),
     * but disable for NAND boot (SPL doesn't use NetBSD boot convention) */
    gxm->prom_emulation = cfg->nand_path ? 0 : 1;
    gxm->boot_kernel_filename = cfg->kernel_path ? strdup(cfg->kernel_path) : strdup("");
    gxm->boot_string_argument = cfg->cmdline ? strdup(cfg->cmdline) : strdup("");

    if (cfg->trace)
        gxm->instruction_trace = 1;

    /*
     * Manually set up the machine (adapted from emul_machine_setup).
     * We skip file_load since we use our own loader.
     */

    /* Create memory */
    uint64_t memory_amount = (uint64_t)gxm->physical_ram_in_mb * 1048576;
    gxm->memory = memory_new(memory_amount);

    /* Create CPU */
    gxm->ncpus = 1;
    gxm->cpus = malloc(sizeof(struct cpu *));
    gxm->cpus[0] = cpu_new(gxm->memory, gxm, 0, gxm->cpu_name);
    if (!gxm->cpus[0]) {
        fprintf(stderr, "[BE300] cpu_new failed for VR4131\n");
        free(m);
        return NULL;
    }
    gxm->bootstrap_cpu = 0;
    m->cpu = gxm->cpus[0];

    /*
     * Run the hpcmips machine setup function.
     * This calls dev_vr41xx_init(), adds the VRC4173 SIU (ns16550),
     * framebuffer, and configures hpc_bootinfo.
     *
     * Disable exit-on-error so missing devices (pcic, etc.) just
     * print a warning instead of crashing.
     */
    device_set_exit_on_error(0);
    machine_setup(gxm);
    device_set_exit_on_error(1);

    /*
     * Initialize our peripheral state structs.
     */
    bcu_init(&m->bcu);
    cmu_init(&m->cmu);
    pmu_init(&m->pmu);
    icu_init(&m->icu);
    siu_init(&m->siu);
    rtc_init(&m->rtc);
    gpio_init(&m->gpio);
    nand_init(&m->nand, NULL, 0);

    /*
     * Load kernel or NAND image.
     */
    if (cfg->kernel_path) {
        if (be300_boot_linux_path(m, cfg->kernel_path, cfg->cmdline,
                                  cfg->ram_path) != 0) {
            free(m);
            return NULL;
        }

    } else if (cfg->nand_path) {
        if (loader_load_nand_image(m, cfg->nand_path) != 0) {
            fprintf(stderr, "[BE300] Failed to load NAND image\n");
            free(m);
            return NULL;
        }

        /* True cold boot: start at ROM reset vector.
         * The ROM will read NAND, load the SPL, run the MIPS16
         * section copier and boot dispatcher, then jump to NK.exe. */
        m->cpu->pc = 0xffffffffBFC00000ULL;
        m->boot_mode = BE300_BOOT_NAND;
        fprintf(stderr, "[BE300] Cold boot: PC=0xBFC00000 (ROM reset vector)\n");

        /* Re-initialize NAND controller with image data */
        if (m->nand_data) {
            nand_init(&m->nand, m->nand_data, m->nand_size);
        }

        /*
         * Map RAM at PA 0x1FC00000 for BEV=1 exception vectors.
         *
         * With prom_emulation=0 (NAND boot), CP0 Status has BEV=1.
         * The SPL doesn't clear BEV, so NK.exe starts with BEV=1.
         * Exception vectors (TLB refill, general, interrupt) go to
         * PA 0x1FC00000 + offset.  Real BE-300 hardware has 16K ROM
         * there (per docs/hardware.txt).  Without this RAM, exception
         * handlers can't be installed and WinCE can't set up virtual
         * memory or run the scheduler.
         */
        dev_ram_init(gxm, 0x1FC00000, 0x4000, DEV_RAM_RAM, 0, NULL);

        /*
         * Load the real BE-300 boot ROM into PA 0x1FC00000.
         * This 16KB masked ROM contains the reset vector, BEV
         * exception handlers, and utility routines that NK.exe
         * may call during initialization.  Captured from real
         * hardware via BEDiag (CRC32=0xFA3B5582).
         *
         */
        {
#include "boot_rom_embedded.h"
            {
                    uint64_t base_va = 0xffffffffBFC00000ULL;
                    for (size_t i = 0; i < be300_boot_rom_len; i += 4) {
                        uint32_t w = be300_boot_rom[i] | (be300_boot_rom[i+1] << 8) |
                                     (be300_boot_rom[i+2] << 16) | (be300_boot_rom[i+3] << 24);
                        store_32bit_word(m->cpu, base_va + i, w);
                    }
                    fprintf(stderr, "[BE300] Loaded embedded boot ROM"
                        " (%u bytes) at PA 0x1FC00000\n", be300_boot_rom_len);

                    /*
                     * Patch the ROM's BEV exception vectors with MIPS32
                     * handlers.  The real ROM has:
                     *   +0x200 (TLB Refill): all 0xFF (no handler)
                     *   +0x380 (General Exception): boot code, not a handler
                     * The ROM uses MIPS16 code that GXemul can't execute.
                     *
                     * We write proper MIPS32 handlers into the 0xFF-filled
                     * area at +0x200-0x37F (384 bytes available).
                     */
                    {
                        uint64_t rom_va = 0xffffffffBFC00000ULL;

                        /* +0x200: BEV TLB Refill handler.
                         * Maps kseg3 (0xC0000000+) to low SDRAM by masking
                         * the upper address bits.  The ROM's MIPS16 dispatcher
                         * uses stack at 0xFF10xxxx which needs to map to
                         * PA 0x00F0xxxx.  Uses VPN2 & 0x1FFFFF as PFN. */
                        /* Use 4KB pages (PageMask=0x1800) to
                         * avoid GXemul's INVALIDATE_ALL on 1KB
                         * TLB writes (R4100 default page size).
                         * EntryLo bits: PFN[25:6] V D G C[5:3]
                         * 0x3F = PFN=0, V=1, D=1, C=3, G=1
                         * Odd page offset: +0x100 (4 x 1KB PFNs
                         * = one 4KB page step in EntryLo PFN). */
                        static const uint32_t tlb_refill[] = {
                            0x401B5000, /* mfc0 $k1, EntryHi    */
                            0x001BD1C2, /* srl  $k0, $k1, 7     */
                            0x3C1B0003, /* lui  $k1, 0x0003     */
                            0x377BFFFF, /* ori  $k1, 0xFFFF     */
                            0x035BD024, /* and  $k0, $k0, $k1   */
                            0x375A003F, /* ori  $k0, $k0, 0x3F  */
                            0x409A1000, /* mtc0 $k0, EntryLo0   */
                            0x275B0100, /* addiu $k1,$k0, 0x100 */
                            0x409B1800, /* mtc0 $k1, EntryLo1   */
                            0x3C1B0000, /* lui  $k1, 0           */
                            0x377B1800, /* ori  $k1, 0x1800      */
                            0x409B2800, /* mtc0 $k1, PageMask    */
                            0x42000006, /* tlbwr                 */
                            0x42000018, /* eret                  */
                        };
                        for (unsigned j = 0; j < sizeof(tlb_refill)/sizeof(tlb_refill[0]); j++)
                            store_32bit_word(m->cpu,
                                rom_va + 0x200 + j * 4,
                                tlb_refill[j]);

                        /*
                         * +0x280: General exception dispatcher.
                         * Checks ExcCode: TLB miss -> identity map,
                         * interrupt -> clear timer + ERET, other -> ERET.
                         *
                         * The BEV general exception vector is at +0x380,
                         * which is the boot code continuation. We can't
                         * easily patch +0x380 (it's a delay slot of the
                         * JAL at +0x37C). Instead, we put the handler at
                         * +0x280 and patch +0x380 to jump here when EXL=1.
                         */
                        /* Actually, let me write a cleaner version */
                        static const uint32_t gen_handler[] = {
                            /* +0x280: general exception handler */
                            0x401A6800, /* mfc0 $k0, Cause        */
                            0x335A007C, /* andi $k0, $k0, 0x7C    */
                            /* k0 now has ExcCode << 2              */
                            /* 0x00 = Interrupt                     */
                            0x13400006, /* beqz $k0, handle_irq (+6)*/
                            0x00000000, /* nop                      */
                            /* 0x08 = TLBL, 0x0C = TLBS            */
                            0x235BFFF8, /* addi $k1, $k0, -8       */
                            0x2F7B0008, /* sltiu $k1, $k1, 8      */
                            /* k1=1 if exccode<<2 is 8..15         */
                            /* i.e. ExcCode 2 or 3 (TLBL/TLBS)     */
                            0x17600005, /* bne $k1, $zero, tlb (+5)*/
                            0x00000000, /* nop                      */
                            /* Other: just ERET                     */
                            0x42000018, /* eret                     */
                            0x00000000, /* nop                      */
                            /* handle_irq: ERET (timer stays pending */
                            /* but at least we return cleanly)       */
                            0x42000018, /* eret                     */
                            0x00000000, /* nop                      */
                            /* handle_tlb: map with addr mask.       */
                            /* Uses tlbp+tlbwi to overwrite existing */
                            /* invalid entries (e.g. zero-initialized*/
                            /* TLB entries matching VPN2=0 with V=0).*/
                            /* Uses 4KB pages (PageMask=0x1800) and
                             * TLBWR (write random) to avoid both:
                             * - overwriting wired entries 0-1
                             *   (TLBWR skips entries below Wired)
                             * - GXemul's INVALIDATE_ALL on 1KB TLB
                             *   writes (which destroys the handler's
                             *   own IC page, preventing ERET)       */
                            0x401B5000, /* mfc0 $k1, EntryHi       */
                            0x001BD1C2, /* srl  $k0, $k1, 7        */
                            0x3C1B1FFF, /* lui  $k1, 0x1FFF        */
                            0x377BFFFF, /* ori  $k1, 0xFFFF        */
                            0x035BD024, /* and  $k0, $k0, $k1      */
                            0x375A003F, /* ori  $k0, $k0, 0x3F     */
                            0x409A1000, /* mtc0 $k0, EntryLo0      */
                            0x275B0100, /* addiu $k1, $k0, 0x100   */
                            0x409B1800, /* mtc0 $k1, EntryLo1      */
                            0x3C1B0000, /* lui  $k1, 0x0000        */
                            0x377B1800, /* ori  $k1, $k1, 0x1800   */
                            0x409B2800, /* mtc0 $k1, PageMask      */
                            0x42000006, /* tlbwr                    */
                            0x42000018, /* eret                     */
                        };
                        /*
                         * The handler body goes at +0x2300 (unused ROM
                         * padding area, 0x2250-0x3FFF) to avoid
                         * overwriting the ROM's dispatch table at +0x2A0
                         * which the MIPS16 boot dispatcher reads.
                         * A jump stub at +0x280 redirects to +0x2300.
                         */
                        for (unsigned j = 0;
                             j < sizeof(gen_handler)/sizeof(gen_handler[0]);
                             j++)
                            store_32bit_word(m->cpu,
                                rom_va + 0x2300 + j * 4,
                                gen_handler[j]);
                        /* +0x280: J 0xBFC02300 (jump to relocated handler) */
                        store_32bit_word(m->cpu, rom_va + 0x280,
                            0x0BF008C0);  /* j 0xBFC02300 */
                        store_32bit_word(m->cpu, rom_va + 0x284,
                            0x00000000);  /* nop (delay slot) */

                        /*
                         * +0x380: Patch the BEV general exception entry.
                         *
                         * The original ROM has boot continuation code
                         * at +0x37C (JAL) with delay slot at +0x380
                         * (`li $a0, 0`).  When an exception fires, the
                         * CPU jumps to +0x380 and the `li $a0, 0`
                         * executes as the FIRST instruction of the
                         * exception handler — clobbering $a0 on every
                         * general exception.
                         *
                         * Since the boot code was relocated to +0x394,
                         * +0x380 is dead code in the normal boot flow.
                         * We replace it with NOP to prevent the $a0
                         * clobber.  The EXL check at +0x384 dispatches
                         * to our handler at +0x280 when an exception
                         * is active, or falls through to the relocated
                         * boot code at +0x394 for normal boot flow.
                         */
                        /* +0x380: NOP (was `li $a0, 0` — clobbered $a0
                         * on every exception entry) */
                        store_32bit_word(m->cpu, rom_va + 0x380,
                            0x00000000); /* nop */
                        /* At +0x384: check EXL */
                        /* Original: lui $a1, 0x8001 (0x3c058001)  */
                        /* Calculate branch offset: from +0x38C to +0x280
                           offset = (0x280 - 0x390) / 4 = -0x110/4 = -68
                           BNE encoding: offset is (target - (PC+4))/4
                           PC = 0x38C, target = 0x280
                           offset = (0x280 - 0x390) / 4 = -0x44 = -68
                           16-bit signed: 0xFFBC */
                        store_32bit_word(m->cpu, rom_va + 0x384,
                            0x401A6000); /* mfc0 $k0, Status */
                        store_32bit_word(m->cpu, rom_va + 0x388,
                            0x335A0002); /* andi $k0, $k0, 2 */
                        store_32bit_word(m->cpu, rom_va + 0x38C,
                            0x1740FFBC); /* bne $k0,$zero, -68 -> +0x280 */
                        store_32bit_word(m->cpu, rom_va + 0x390,
                            0x00000000); /* nop (delay slot) */
                        /* Original boot code relocated from +0x384: */
                        store_32bit_word(m->cpu, rom_va + 0x394,
                            0x3C058001); /* lui $a1, 0x8001 (was +0x384) */
                        store_32bit_word(m->cpu, rom_va + 0x398,
                            0x24A50034); /* addiu $a1, 52 (was +0x388) */
                        store_32bit_word(m->cpu, rom_va + 0x39C,
                            0x80A00000); /* lb $zero, 0($a1) (was +0x38C) */
                        store_32bit_word(m->cpu, rom_va + 0x3A0,
                            0x3C089FC0); /* lui $t0, 0x9FC0 (was +0x390) */
                        store_32bit_word(m->cpu, rom_va + 0x3A4,
                            0x25080C85); /* addiu $t0, 0xC85 (was +0x394) */
                        store_32bit_word(m->cpu, rom_va + 0x3A8,
                            0x0100F809); /* jalr $t0 (was +0x398) */
                        store_32bit_word(m->cpu, rom_va + 0x3AC,
                            0x00000000); /* nop (was +0x39C) */
                        store_32bit_word(m->cpu, rom_va + 0x3B0,
                            0x3C059FC0); /* lui $a1, 0x9FC0 (was +0x3A0) */
                        store_32bit_word(m->cpu, rom_va + 0x3B4,
                            0x24A50C21); /* addiu $a1, 0xC21 (was +0x3A4) */
                        store_32bit_word(m->cpu, rom_va + 0x3B8,
                            0x00A0F809); /* jalr $a1 (was +0x3A8) */
                        store_32bit_word(m->cpu, rom_va + 0x3BC,
                            0x00000000); /* nop (was +0x3AC) */
                        store_32bit_word(m->cpu, rom_va + 0x3C0,
                            0x0FF0013A); /* jal 0xFC004E8 (was +0x3B0) */
                        store_32bit_word(m->cpu, rom_va + 0x3C4,
                            0x00000000); /* nop (was +0x3B4) */
                        store_32bit_word(m->cpu, rom_va + 0x3C8,
                            0x0FF00122); /* jal 0xFC00488 (was +0x3B8) */
                        store_32bit_word(m->cpu, rom_va + 0x3CC,
                            0x00000000); /* nop (was +0x3BC) */
                        /* beq loop: was `beq v0, zero, 0x3A0` at +0x3C0.
                           Now relocated to +0x3D0, target is +0x3B0
                           (the boot dispatcher, NOT the section copier
                           at +0x3A0).
                           offset = (0x3B0 - 0x3D4) / 4 = -9 = 0xFFF7 */
                        store_32bit_word(m->cpu, rom_va + 0x3D0,
                            0x1040FFF7); /* beq v0, zero, 0x3B0 */
                        store_32bit_word(m->cpu, rom_va + 0x3D4,
                            0x00000000); /* nop (delay slot) */
                        /* FUN_9fc003dc (recursive DMA dispatcher) at
                         * +0x3DC has `j 0x3A0` at +0x3E4.  After the
                         * relocation, +0x3A0 is the section copier
                         * setup, not the boot dispatcher.  Update the
                         * J target to +0x3B0 (boot dispatcher) so the
                         * recursive path skips the section copier and
                         * goes directly to the boot retry loop. */
                        store_32bit_word(m->cpu, rom_va + 0x3E4,
                            0x0BF000EC); /* j 0x9FC003B0 */
                        /* NK.exe jump trampoline at +0x2360 (after
                         * gen_handler which ends at +0x235C) */
                        store_32bit_word(m->cpu, rom_va + 0x2360,
                            0x3C08A006); /* lui $t0, 0xA006 */
                        store_32bit_word(m->cpu, rom_va + 0x2364,
                            0x25080004); /* addiu $t0, 0x0004 */
                        store_32bit_word(m->cpu, rom_va + 0x2368,
                            0x01000008); /* jr $t0 (-> 0xA0060004) */
                        store_32bit_word(m->cpu, rom_va + 0x236C,
                            0x00000000); /* nop */
                        /* Error recovery path: */
                        store_32bit_word(m->cpu, rom_va + 0x3EC,
                            0x0FF00126); /* jal 0xFC00498 (was +0x3DC) */
                        store_32bit_word(m->cpu, rom_va + 0x3F0,
                            0x24040004); /* li $a0, 4 (was +0x3E0) */
                        store_32bit_word(m->cpu, rom_va + 0x3F4,
                            0x0BF000E8); /* j 0xFC003A0 -> +0x3B0 relocated */
                        store_32bit_word(m->cpu, rom_va + 0x3F8,
                            0x00000000); /* nop */

                        /*
                         * WORKAROUND: Patch delay-loop SUB→SUBU at
                         * ROM +0x51C and +0x520.
                         *
                         * The ROM's MIPS32 delay function at offset
                         * 0x534 reads CP0 Count, subtracts a base
                         * value, and compares against a threshold.
                         * It uses SUB (which traps on signed overflow)
                         * rather than SUBU (which wraps silently).
                         *
                         * In the emulator, CP0 Count can wrap past
                         * 0x80000000 between reads because we reset
                         * Count to 0 on DMA triggers (another
                         * workaround).  The SUB then computes a
                         * negative difference that exceeds the signed
                         * range, triggering an Integer Overflow
                         * exception.  On real hardware, Count
                         * increments monotonically and the difference
                         * stays small, so the overflow never occurs.
                         *
                         * SUBU produces the same arithmetic result
                         * without trapping.
                         */
                        store_32bit_word(m->cpu, rom_va + 0x51C,
                            0x00C53023); /* WORKAROUND: subu a2,a2,a1 */
                        store_32bit_word(m->cpu, rom_va + 0x520,
                            0x00461023); /* WORKAROUND: subu v0,v0,a2 */

                        fprintf(stderr, "[BE300] Patched ROM BEV"
                            " vectors: TLB@+0x200, GenExc@+0x2300"
                            " (stub@+0x280),"
                            " boot code relocated +0x384->+0x394\n");
                    }

                    /*
                     * NOTE: The NAND device descriptor base pointer at
                     * PA 0x10030 (VA 0x80010030) is needed by device 2's
                     * init function.  It must be set AFTER the SDRAM test
                     * and device 1 init populate the table at 0x80010040.
                     * This is handled by a PC-gated fixup in the MIPS16
                     * interpreter (cpu_mips16.c) when FUN_9fc019d4 runs.
                     */
                }
        }

        /*
         * Pre-load TLB with mappings for VRC4173 I/O access.
         * The ROM's MIPS16 dispatcher uses kseg3 virtual addresses
         * that need TLB mappings:
         *
         *   VA 0xFF100000 -> PA 0x09100000 (on-chip SRAM)
         *   VA 0xF2000000 -> PA 0x0A000000 (VRC4173 I/O)
         */
        {
            uint64_t *cp0 = m->cpu->cd.mips.coproc[0]->reg;

            /* TLB 0: VA 0xFF100000 -> PA 0x09100000 (stack)
             * Use 4KB pages (PageMask=0x1800) to avoid GXemul's
             * INVALIDATE_ALL on R4100 1KB TLB entries.           */
            cp0[COP0_PAGEMASK] = 0x1800;  /* 4KB pages (R4100) */
            cp0[COP0_ENTRYHI]  = 0xFF100000ULL;
            cp0[COP0_ENTRYLO0] = (0x09100000u >> 12) << 6 | 0x17;
            cp0[COP0_ENTRYLO1] = (0x09101000u >> 12) << 6 | 0x17;
            cp0[COP0_INDEX]    = 0;
            coproc_tlbwri(m->cpu, 0);

            /* TLB 1: VA 0xF2000000 -> PA 0x0A000000 (VRC4173)
             * Use 256KB pages to cover 0xF2000000-0xF207FFFF */
            cp0[COP0_PAGEMASK] = 0x0007F800; /* 256KB pages (VR4131: shift=11) */
            cp0[COP0_ENTRYHI]  = 0xF2000000ULL;
            cp0[COP0_ENTRYLO0] = (0x0A000000u >> 12) << 6 | 0x17;
            cp0[COP0_ENTRYLO1] = (0x0A040000u >> 12) << 6 | 0x17;
            cp0[COP0_INDEX]    = 1;
            coproc_tlbwri(m->cpu, 0);

            cp0[COP0_WIRED]    = 2;
            cp0[COP0_PAGEMASK] = 0;  /* reset */
            fprintf(stderr,
                "[BE300] TLB: 0xFF100000->PA 0x09100000,"
                " 0xF2000000->PA 0x0A000000\n");
        }

        /* Register VRC4173 latch (catch-all); pre-split to leave gaps for input device */
        extern void be300_register_vrc4173_latch(struct machine *, bool);
        be300_register_vrc4173_latch(gxm, cfg->log_mmio);

        /* Register NAND flash; pre-split to leave gap at 0x0A00A040 for input device */
        extern void be300_register_nand(struct machine *, nand_state_t *, bool);
        be300_register_nand(gxm, &m->nand, cfg->log_mmio);

        /* Register input devices AFTER latch/NAND (fills pre-carved gaps) */
        extern void be300_register_input(struct machine *, machine_t *, bool);
        be300_register_input(gxm, m, cfg->log_mmio);
        m->input_registered = true;
        wince_boot_note_spl_handoff(m);

    } else if (cfg->rom_path) {
        if (loader_load_rom(m, cfg->rom_path) != 0) {
            fprintf(stderr, "[BE300] Failed to load ROM image\n");
            free(m);
            return NULL;
        }
        m->boot_mode = BE300_BOOT_ROM;
    }

    wince_boot_attach_machine(m);

    fprintf(stderr, "[BE300] Machine created: %u MB SDRAM, VR4131 CPU\n",
            cfg->sdram_size / (1024 * 1024));
    return m;
}


static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void be300_runtime_start(machine_t *m)
{
    struct machine *gxm = m->gxe_machine;
    struct emul *emul = m->emul;

    if (m->runtime_initialized)
        return;

    fprintf(stderr, "[BE300] Starting emulation at PC=0x%08" PRIx64 "\n",
            m->cpu->pc);

    cpu_run_init(gxm);
    m->cpu->running = true;

    if (m->web_mode)
        timer_reset_state();
    else
        timer_start();
    console_init_main(emul);

    if (m->use_builtin_ui)
        ui_init(m);

    if (m->cfg.target_mhz > 0) {
        if (m->web_mode)
            fprintf(stderr, "[BE300] Web pacing target: %u MHz-equivalent\n",
                    m->cfg.target_mhz);
        else
            fprintf(stderr, "[BE300] Throttle: targeting %u MHz\n",
                    m->cfg.target_mhz);
    } else {
        fprintf(stderr, "[BE300] Throttle: disabled (unthrottled)\n");
    }

    m->throttle_target_ips = (uint64_t)m->cfg.target_mhz * 1000000ULL;
    m->throttle_wall_origin = 0;
    m->throttle_instr_origin = 0;
    m->last_report = 0;
    m->loop_count = 0;
    m->runtime_initialized = true;
    m->runtime_stopped = false;
    m->runtime_finalized = false;
    emul_executing = true;

    signal(SIGTERM, be300_handle_stop_signal);
    if (!m->web_mode)
        signal(SIGINT, be300_handle_stop_signal);

    host_io_set_serial_sink(be300_serial_sink, m);
    host_io_set_stdout_enabled(m->mirror_serial_to_stdout);
}

static void be300_runtime_finalize(machine_t *m)
{
    if (!m || !m->runtime_initialized || m->runtime_finalized)
        return;

    if (m->save_exit_screenshot)
        ui_save_screenshot(m);

    if (m->use_builtin_ui)
        ui_destroy(m);

    timer_stop();
    host_io_set_serial_sink(NULL, NULL);
    host_io_set_stdout_enabled(true);

    emul_executing = false;
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    cpu_run_deinit(m->gxe_machine);
    m->runtime_stopped = true;
    m->runtime_finalized = true;

    fprintf(stderr, "[BE300] Emulation stopped after %" PRIi64 " instructions\n",
            m->cpu->ninstrs);
}

static bool be300_run_batch(machine_t *m)
{
    struct machine *gxm = m->gxe_machine;

    __sync_synchronize();
    if (emul_shutdown) {
        fprintf(stderr, "[BE300] Loop exit: emul_shutdown is true\n");
        return false;
    }

    if (m->loop_count % 1000 == 0) {
        fprintf(stderr, "[BE300] Loop batch %d, PC=0x%08" PRIx64 "\n",
                m->loop_count, m->cpu->pc);
    }

    /* Detect user-mode execution (PC in kuseg: 0x00000000-0x7FFFFFFF) */
    {
        static int usermode_logged = 0;
        uint64_t pc = m->cpu->pc;
        if (!usermode_logged && pc < 0x80000000ULL && pc > 0x1000ULL) {
            fprintf(stderr, "[BE300] *** USER MODE DETECTED: PC=0x%08" PRIx64 " ***\n", pc);
            usermode_logged = 1;
        }
    }

    /* (debug instrumentation removed) */

    /* Detect SPL entry: PC in range 0x80F00000-0x80F0FFFF (PA 0xF00000) */
    {
        static int spl_entry_logged = 0;
        static int spl_probe_done = 0;
        uint32_t pc = (uint32_t)m->cpu->pc;
        uint32_t pa = pc & 0x1FFFFFFFu;
        if (!spl_entry_logged && pa >= 0xF00000u && pa < 0xF10000u) {
            fprintf(stderr,
                    "[BE300] *** SPL ENTRY DETECTED: PC=0x%08X PA=0x%08X batch=%d ***\n",
                    pc, pa, m->loop_count);
            spl_entry_logged = 1;
        }
        /* Periodically probe SPL memory to check if B000FF loaded */
        if (m->loop_count > 0 && m->loop_count % 10000 == 0 && m->loop_count <= 100000) {
            uint8_t buf[16];
            uint64_t spl_va = 0xffffffff80F00000ULL;
            if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                    spl_va, buf, 16, MEM_READ, CACHE_DATA)) {
                fprintf(stderr,
                        "[BE300] SPL PROBE PA=0xF00000: "
                        "%02X %02X %02X %02X %02X %02X %02X %02X "
                        "%02X %02X %02X %02X %02X %02X %02X %02X\n",
                        buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7],
                        buf[8],buf[9],buf[10],buf[11],buf[12],buf[13],buf[14],buf[15]);
            } else {
                fprintf(stderr, "[BE300] SPL PROBE PA=0xF00000: memory_rw FAILED\n");
            }
            /* Also probe the entry point at 0x80F00004 */
            spl_va = 0xffffffff80F00004ULL;
            if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                    spl_va, buf, 4, MEM_READ, CACHE_DATA)) {
                uint32_t instr = buf[0] | (buf[1]<<8) | (buf[2]<<16) | (buf[3]<<24);
                fprintf(stderr, "[BE300] SPL PROBE PA=0xF00004: instruction=0x%08X\n", instr);
            }
            /* Probe PA 0x24FC */
            spl_va = 0xffffffffA00024FCULL;
            if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                    spl_va, buf, 4, MEM_READ, CACHE_DATA)) {
                uint32_t val = buf[0] | (buf[1]<<8) | (buf[2]<<16) | (buf[3]<<24);
                fprintf(stderr, "[BE300] ENTRY PROBE PA=0x24FC: val=0x%08X\n", val);
            }
        }
    }

    /*
     * Detect NK.exe entry and dump decompressed binary for analysis.
     * The SPL decompresses NK.exe to PA 0x60000 and jumps to
     * 0xA0060004 which redirects to 0x80076B50.
     */
    if (!m->wince.cold_boot_copy_done) {
        uint32_t pc = (uint32_t)m->cpu->pc;
        uint32_t pa = pc & 0x1FFFFFFFu;
        if (pa >= 0x60000u && pa < 0x100000u) {
            m->wince.cold_boot_copy_done = true;
            m->nand.wince_mode = true;

            /* Dump decompressed NK.exe binary for analysis */
            {
                const uint32_t nk_pa = 0x00060000u;
                const uint32_t nk_size = 0x005F6AC8u;
                FILE *nk_fp = fopen("nk_decompressed.bin", "wb");
                if (nk_fp) {
                    uint8_t page[4096];
                    uint32_t off = 0;
                    while (off < nk_size) {
                        uint32_t chunk = nk_size - off;
                        if (chunk > sizeof(page)) chunk = sizeof(page);
                        uint64_t va = 0xffffffff80000000ULL | (nk_pa + off);
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                va, page, chunk, MEM_READ, CACHE_DATA))
                            fwrite(page, 1, chunk, nk_fp);
                        else
                            break;
                        off += chunk;
                    }
                    fclose(nk_fp);
                    fprintf(stderr,
                        "[BE300] Dumped NK.exe (%u bytes)"
                        " to nk_decompressed.bin\n", off);
                }
            }
        }
    }

    if (!machine_run(gxm)) {
        wince_boot_note_fatal_stop(m, "machine-no-longer-running");
        fprintf(stderr, "[BE300] Loop exit: machine no longer running"
            " (mips16=%d halted=%d PC=0x%08X)\n",
            m->cpu->cd.mips.mips16,
            m->cpu->is_halted,
            (uint32_t)m->cpu->pc);

        /* Basic crash diagnostics */
        {
            uint32_t pc = (uint32_t)m->cpu->pc;
            fprintf(stderr, "[CRASH] PC=0x%08X\n", pc);
            fprintf(stderr, "[CRASH] CP0: Status=0x%08X"
                " Cause=0x%08X EPC=0x%08X BadVA=0x%08X\n",
                (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS],
                (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE],
                (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC],
                (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_BADVADDR]);
            fprintf(stderr, "[CRASH] GPR:"
                " sp=%08X fp=%08X ra=%08X gp=%08X\n",
                (uint32_t)m->cpu->cd.mips.gpr[29],
                (uint32_t)m->cpu->cd.mips.gpr[30],
                (uint32_t)m->cpu->cd.mips.gpr[31],
                (uint32_t)m->cpu->cd.mips.gpr[28]);
        }

        return false;
    }

    if (m->web_mode)
        timer_tick_manual();

    if (m->loop_count % 1000 == 0) {
        fprintf(stderr, "[BE300] Loop batch %d done\n", m->loop_count);
    }

    if (!m->web_mode && m->throttle_target_ips > 0) {
        uint64_t now_ns = monotonic_ns();
        uint64_t instrs  = (uint64_t)m->cpu->ninstrs;

        if (m->throttle_wall_origin == 0) {
            m->throttle_wall_origin = now_ns;
            m->throttle_instr_origin = instrs;
        } else {
            uint64_t elapsed_instrs = instrs - m->throttle_instr_origin;
            uint64_t target_ns = (elapsed_instrs * 1000000000ULL)
                                 / m->throttle_target_ips;
            uint64_t actual_ns = now_ns - m->throttle_wall_origin;

            if (target_ns > actual_ns) {
                uint64_t sleep_ns = target_ns - actual_ns;
                struct timespec ts;

                if (sleep_ns > 50000000ULL)
                    sleep_ns = 50000000ULL;

                ts.tv_sec = (time_t)(sleep_ns / 1000000000ULL);
                ts.tv_nsec = (long)(sleep_ns % 1000000000ULL);
                nanosleep(&ts, NULL);
            }

            if (elapsed_instrs >= 1000000ULL) {
                m->throttle_wall_origin = monotonic_ns();
                m->throttle_instr_origin = instrs;
            }
        }
    }

    console_flush();
    if (m->use_builtin_ui)
        ui_update(m);

    if (m->use_builtin_ui && ui_should_quit(m)) {
        fprintf(stderr, "[BE300] Loop exit: ui_should_quit is true\n");
        return false;
    }

    if (m->cpu->ninstrs - m->last_report >= 50000000LL) {
        uint64_t *cp0 = m->cpu->cd.mips.coproc[0]->reg;
        fprintf(stderr, "[BE300] Progress: %" PRIi64 "M instrs,"
            " PC=0x%08" PRIx64
            " Status=0x%08X Cause=0x%08X EPC=0x%08X"
            " BadVA=0x%08X SP=0x%08X RA=0x%08X\n",
            m->cpu->ninstrs / 1000000LL, m->cpu->pc,
            (uint32_t)cp0[COP0_STATUS],
            (uint32_t)cp0[COP0_CAUSE],
            (uint32_t)cp0[COP0_EPC],
            (uint32_t)cp0[COP0_BADVADDR],
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA]);
        m->last_report = m->cpu->ninstrs;

        /* Check PA 0x24FC and version at PA 0x2400 */
        {
            uint8_t buf[4];
            uint64_t va;
            va = 0xffffffffA0002400ULL;
            if (m->cpu->memory_rw(m->cpu, m->cpu->mem, va, buf, 4, MEM_READ, CACHE_DATA)) {
                uint32_t ver = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
                va = 0xffffffffA00024FCULL;
                m->cpu->memory_rw(m->cpu, m->cpu->mem, va, buf, 4, MEM_READ, CACHE_DATA);
                uint32_t ep = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
                fprintf(stderr, "[BE300]   PA_2400=0x%08X PA_24FC=0x%08X\n", ver, ep);
            }
        }
    }

    if (++m->loop_count % 100 == 0) {
        /* keep hook point for extremely verbose diagnostics */
    }

    return true;
}

int be300_step(machine_t *m, uint32_t max_batches)
{
    if (!m)
        return -1;
    if (m->boot_mode == BE300_BOOT_NONE) {
        fprintf(stderr, "[BE300] Cannot run before boot media is loaded\n");
        return -1;
    }
    if (m->runtime_finalized)
        return 0;
    if (max_batches == 0)
        max_batches = 1;

    be300_runtime_start(m);

    for (uint32_t i = 0; i < max_batches; i++) {
        if (!be300_run_batch(m)) {
            be300_runtime_finalize(m);
            return 0;
        }
    }

    return 1;
}

void be300_run(machine_t *m)
{
    while (be300_step(m, 1) > 0) {
    }
}

machine_t *be300_create_web(uint32_t sdram_mb, uint32_t target_mhz,
                            bool sfb_5bit_green)
{
    machine_config_t cfg = {
        .trace = false,
        .log_mmio = false,
        .sfb_5bit_green = sfb_5bit_green,
        .log_nand_legacy = false,
        .rom_path = NULL,
        .kernel_path = NULL,
        .cmdline = NULL,
        .ram_path = NULL,
        .nand_path = NULL,
        .sdram_size = (sdram_mb ? sdram_mb : 16u) * 1024u * 1024u,
        .target_mhz = target_mhz,
    };
    machine_t *m = be300_create(&cfg);
    if (!m)
        return NULL;

    m->web_mode = true;
    m->use_builtin_ui = false;
    m->save_exit_screenshot = false;
    m->mirror_serial_to_stdout = false;
    return m;
}

int be300_boot_linux_from_memory(machine_t *m,
                                 const void *kernel_data,
                                 size_t kernel_size,
                                 const char *cmdline)
{
    return be300_boot_linux_memory_internal(m, kernel_data, kernel_size, cmdline);
}

int be300_copy_frame_rgba8888(machine_t *m, uint8_t *dst, size_t dst_len,
                              uint32_t *width_out, uint32_t *height_out)
{
    const uint16_t *src;
    uint32_t width;
    uint32_t height;

    if (!m || !dst || !width_out || !height_out)
        return -1;

    width = m->fb_width ? m->fb_width : 240u;
    height = m->fb_height ? m->fb_height : 320u;
    if (dst_len < (size_t)width * height * 4u)
        return -1;

    if (!m->fb_data) {
        if (!m->gxe_machine || !m->gxe_machine->fb || !m->gxe_machine->fb->framebuffer)
            return 0;
        m->fb_data = m->gxe_machine->fb->framebuffer;
    }

    src = (const uint16_t *)m->fb_data;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint16_t pixel = src[y * m->fb_stride + x];
            uint32_t r = (pixel >> 11) & 0x1Fu;
            uint32_t b = pixel & 0x1Fu;
            uint32_t g;
            size_t off = ((size_t)y * width + x) * 4u;

            if (m->cfg.sfb_5bit_green) {
                g = (pixel >> 5) & 0x1Fu;
                g = (g << 3) | (g >> 2);
            } else {
                g = (pixel >> 5) & 0x3Fu;
                g = (g << 2) | (g >> 4);
            }

            r = (r << 3) | (r >> 2);
            b = (b << 3) | (b >> 2);

            dst[off + 0] = (uint8_t)r;
            dst[off + 1] = (uint8_t)g;
            dst[off + 2] = (uint8_t)b;
            dst[off + 3] = 0xFFu;
        }
    }

    *width_out = width;
    *height_out = height;
    return 1;
}

size_t be300_drain_serial(machine_t *m, char *dst, size_t dst_len)
{
    size_t count = 0;

    if (!m || !dst || dst_len == 0)
        return 0;

    while (count < dst_len && m->serial_count > 0) {
        dst[count++] = m->serial_ring[m->serial_tail];
        m->serial_tail = (m->serial_tail + 1u) % BE300_SERIAL_RING_CAP;
        m->serial_count--;
    }

    return count;
}

void be300_set_touch(machine_t *m, bool down, uint16_t x, uint16_t y)
{
    if (!m)
        return;
    m->touch_down = down;
    m->touch_x = x;
    m->touch_y = y;
    __sync_synchronize();
}

void be300_set_buttons(machine_t *m, uint8_t btn_set1, uint8_t btn_set2)
{
    if (!m)
        return;
    m->btn_set1 = btn_set1;
    m->btn_set2 = btn_set2;
    __sync_synchronize();
}

void be300_stop(machine_t *m)
{
    (void)m;
    emul_shutdown = true;
    __sync_synchronize();
}


void be300_destroy(machine_t *m)
{
    if (!m) return;

    be300_stop(m);
    be300_runtime_finalize(m);

    wince_boot_detach_machine(m);

    if (m->nand_data) {
        free(m->nand_data);
        m->nand_data = NULL;
    }

    free(m);

    /*
     * Clear GXemul global state so that be300_create() can be called again.
     * interrupt_handler_register() aborts with exit(1) on duplicate names,
     * and timer_add() appends to a global list — both must be reset between
     * machine instantiations (e.g. kernel switch from the Android UI).
     */
    interrupt_handler_clear_all();
    timer_remove_all();
}
