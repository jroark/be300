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
#include "ppsh.h"
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
extern bool single_step;

static bool be300_restore_main_trace_active = false;
static bool be300_restore_main_trace_done = false;
static unsigned be300_restore_main_trace_remaining = 0;

static uint32_t be300_canonicalize_nk_pc(uint32_t pc)
{
    if ((pc & 0xE0000000u) == 0x80000000u
        || (pc & 0xE0000000u) == 0xA0000000u)
        return (pc & 0x1FFFFFFFu) | 0x80000000u;
    return pc;
}

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

static void be300_ppsh_poll_host_input(machine_t *m)
{
    unsigned char raw[128];
    uint8_t cooked[256];
    size_t raw_len;
    size_t cooked_len = 0;
    size_t queued;

    if (!m || !m->cfg.enable_ppsh || !be300_ppsh_transport_ready())
        return;

    raw_len = host_io_read_stdin_nonblocking(raw, sizeof(raw));
    if (raw_len == 0)
        return;

    for (size_t i = 0; i < raw_len && cooked_len < sizeof(cooked); i++) {
        unsigned char ch = raw[i];

        if (ch == '\n') {
            if (cooked_len == 0 || cooked[cooked_len - 1] != '\r')
                cooked[cooked_len++] = '\r';
        } else {
            cooked[cooked_len++] = ch;
        }
    }

    queued = be300_ppsh_queue_host_input(cooked, cooked_len);
    if (m->cfg.log_mmio && queued > 0) {
        fprintf(stderr,
            "[PPSH] queued %zu host byte%s for guest transport\n",
            queued, queued == 1 ? "" : "s");
    }
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

static bool be300_uses_rom_boot(const machine_config_t *cfg)
{
    return cfg && (cfg->nand_path || cfg->restore);
}

static bool be300_nand_image_looks_bootable(const machine_t *m)
{
    static const uint8_t spl_sig[] = { 'B', '0', '0', '0', 'F', 'F', '\n' };

    if (!m || !m->nand_data || m->nand_size < 0x4000u + sizeof(spl_sig))
        return false;

    return memcmp(m->nand_data + 0x4000u, spl_sig, sizeof(spl_sig)) == 0;
}

/*
 *  The masked ROM does not provide usable BEV exception handlers at +0x200
 *  and +0x380 for the cold-boot path we emulate. Install MIPS32 stubs there
 *  so early ROM TLB faults can refill and return cleanly while the original
 *  boot flow continues through relocated instructions.
 */
static void be300_patch_boot_rom_vectors(machine_t *m)
{
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
        0x3C1B0000, /* lui  $k1, 0x0000     */
        0x377B1800, /* ori  $k1, 0x1800     */
        0x409B2800, /* mtc0 $k1, PageMask   */
        0x42000006, /* tlbwr                */
        0x42000018, /* eret                 */
    };
    static const uint32_t gen_handler[] = {
        0x401A6800, /* mfc0 $k0, Cause        */
        0x335A007C, /* andi $k0, $k0, 0x7C    */
        0x13400006, /* beqz $k0, handle_irq   */
        0x00000000, /* nop                    */
        0x235BFFF8, /* addi $k1, $k0, -8      */
        0x2F7B0008, /* sltiu $k1, $k1, 8      */
        0x17600005, /* bne $k1, $zero, tlb    */
        0x00000000, /* nop                    */
        0x42000018, /* eret                   */
        0x00000000, /* nop                    */
        0x42000018, /* eret                   */
        0x00000000, /* nop                    */
        0x401B5000, /* mfc0 $k1, EntryHi      */
        0x001BD1C2, /* srl  $k0, $k1, 7       */
        0x3C1B1FFF, /* lui  $k1, 0x1FFF       */
        0x377BFFFF, /* ori  $k1, 0xFFFF       */
        0x035BD024, /* and  $k0, $k0, $k1     */
        0x375A003F, /* ori  $k0, $k0, 0x3F    */
        0x409A1000, /* mtc0 $k0, EntryLo0     */
        0x275B0100, /* addiu $k1, $k0, 0x100  */
        0x409B1800, /* mtc0 $k1, EntryLo1     */
        0x3C1B0000, /* lui  $k1, 0x0000       */
        0x377B1800, /* ori  $k1, 0x1800       */
        0x409B2800, /* mtc0 $k1, PageMask     */
        0x42000006, /* tlbwr                  */
        0x42000018, /* eret                   */
    };
    uint64_t rom_va;
    size_t j;

    if (!m || !m->cpu)
        return;

    rom_va = 0xffffffffBFC00000ULL;

    for (j = 0; j < sizeof(tlb_refill) / sizeof(tlb_refill[0]); j++) {
        store_32bit_word(m->cpu, rom_va + 0x200 + j * 4, tlb_refill[j]);
    }

    for (j = 0; j < sizeof(gen_handler) / sizeof(gen_handler[0]); j++) {
        store_32bit_word(m->cpu, rom_va + 0x2300 + j * 4, gen_handler[j]);
    }

    store_32bit_word(m->cpu, rom_va + 0x280, 0x0BF008C0); /* j 0xBFC02300 */
    store_32bit_word(m->cpu, rom_va + 0x284, 0x00000000); /* nop */

    store_32bit_word(m->cpu, rom_va + 0x380, 0x00000000); /* nop */
    store_32bit_word(m->cpu, rom_va + 0x384, 0x401A6000); /* mfc0 $k0, Status */
    store_32bit_word(m->cpu, rom_va + 0x388, 0x335A0002); /* andi $k0, $k0, 2 */
    store_32bit_word(m->cpu, rom_va + 0x38C, 0x1740FFBC); /* bne $k0,$zero,+0x280 */
    store_32bit_word(m->cpu, rom_va + 0x390, 0x00000000); /* nop */

    store_32bit_word(m->cpu, rom_va + 0x394, 0x3C058001);
    store_32bit_word(m->cpu, rom_va + 0x398, 0x24A50034);
    store_32bit_word(m->cpu, rom_va + 0x39C, 0x80A00000);
    store_32bit_word(m->cpu, rom_va + 0x3A0, 0x3C089FC0);
    store_32bit_word(m->cpu, rom_va + 0x3A4, 0x25080C85);
    store_32bit_word(m->cpu, rom_va + 0x3A8, 0x0100F809);
    store_32bit_word(m->cpu, rom_va + 0x3AC, 0x00000000);
    store_32bit_word(m->cpu, rom_va + 0x3B0, 0x3C059FC0);
    store_32bit_word(m->cpu, rom_va + 0x3B4, 0x24A50C21);
    store_32bit_word(m->cpu, rom_va + 0x3B8, 0x00A0F809);
    store_32bit_word(m->cpu, rom_va + 0x3BC, 0x00000000);
    store_32bit_word(m->cpu, rom_va + 0x3C0, 0x0FF0013A);
    store_32bit_word(m->cpu, rom_va + 0x3C4, 0x00000000);
    store_32bit_word(m->cpu, rom_va + 0x3C8, 0x0FF00122);
    store_32bit_word(m->cpu, rom_va + 0x3CC, 0x00000000);
    store_32bit_word(m->cpu, rom_va + 0x3D0, 0x1040FFF7);
    store_32bit_word(m->cpu, rom_va + 0x3D4, 0x8D080000);
    store_32bit_word(m->cpu, rom_va + 0x3D8, 0x01000008);
    store_32bit_word(m->cpu, rom_va + 0x3DC, 0x00000000);
    store_32bit_word(m->cpu, rom_va + 0x3E4, 0x0BF000EC);
    store_32bit_word(m->cpu, rom_va + 0x2360, 0x3C08A006);
    store_32bit_word(m->cpu, rom_va + 0x2364, 0x25080004);
    store_32bit_word(m->cpu, rom_va + 0x2368, 0x01000008);
    store_32bit_word(m->cpu, rom_va + 0x236C, 0x00000000);
    store_32bit_word(m->cpu, rom_va + 0x3EC, 0x0FF00126);
    store_32bit_word(m->cpu, rom_va + 0x3F0, 0x24040004);
    store_32bit_word(m->cpu, rom_va + 0x3F4, 0x0BF000E8);
    store_32bit_word(m->cpu, rom_va + 0x3F8, 0x00000000);

    fprintf(stderr,
        "[BE300] Patched boot ROM BEV vectors (+0x200 refill, +0x2300 general, +0x380 redirect)\n");
}

static bool be300_read_guest_cstring(machine_t *m, uint32_t va,
                                     char *out, size_t out_sz)
{
    size_t pos = 0;

    if (!m || !m->cpu || !out || out_sz == 0)
        return false;

    while (pos + 1 < out_sz) {
        uint8_t ch = 0;
        uint64_t gva = 0xFFFFFFFF00000000ULL | (uint64_t)va;

        if (!m->cpu->memory_rw(m->cpu, m->cpu->mem, gva, &ch, 1,
                MEM_READ, CACHE_DATA | NO_EXCEPTIONS))
            return false;

        out[pos++] = (char)ch;
        va++;
        if (ch == '\0')
            return true;
    }

    out[out_sz - 1] = '\0';
    return true;
}

static void be300_trace_restore_format(machine_t *m)
{
    static uint32_t last_ra = 0;
    static unsigned log_count = 0;
    uint32_t pc;
    uint32_t ra;
    uint32_t fmt_va;
    uint32_t a1;
    uint32_t a2;
    uint32_t a3;
    char fmt[160];

    if (!m || m->boot_mode != BE300_BOOT_RESTORE || log_count >= 256)
        return;

    pc = be300_canonicalize_nk_pc((uint32_t)m->cpu->pc);
    if (pc != 0x80E04428u)
        return;

    ra = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA];
    if (ra == last_ra)
        return;
    last_ra = ra;

    fmt_va = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_A0];
    a1 = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_A1];
    a2 = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_A2];
    a3 = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_A3];

    if (!be300_read_guest_cstring(m, fmt_va, fmt, sizeof(fmt)))
        snprintf(fmt, sizeof(fmt), "<unreadable @0x%08X>", fmt_va);

    fprintf(stderr,
        "[RESTORE_FMT] ra=0x%08X fmt=\"%s\" a1=0x%08X a2=0x%08X a3=0x%08X\n",
        ra, fmt, a1, a2, a3);
    log_count++;
}

static void be300_trace_restore_checkpoints(machine_t *m)
{
    static const struct {
        uint32_t pc;
        const char *name;
    } checkpoints[] = {
        { 0x80E03374u, "area_lookup_ret" },
        { 0x80E033D8u, "all_nand_lookup_ret" },
        { 0x80E034D8u, "have_all_nand" },
        { 0x80E03770u, "volume_read_ret" },
        { 0x80E03DF4u, "nandcoldboot_enter" },
        { 0x80E03E00u, "restore_return" },
        { 0x80E04364u, "main_result_check" },
    };
    static uint32_t seen[(sizeof(checkpoints) / sizeof(checkpoints[0]))];
    uint32_t pc;
    size_t i;

    if (!m || m->boot_mode != BE300_BOOT_RESTORE)
        return;

    pc = be300_canonicalize_nk_pc((uint32_t)m->cpu->pc);
    for (i = 0; i < sizeof(checkpoints) / sizeof(checkpoints[0]); i++) {
        if (pc != checkpoints[i].pc || seen[i] == pc)
            continue;

        seen[i] = pc;
        fprintf(stderr,
            "[RESTORE_CKPT] %s pc=0x%08X v0=0x%08X v1=0x%08X"
            " a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X"
            " s0=0x%08X s1=0x%08X s6=0x%08X s7=0x%08X"
            " ra=0x%08X sp=0x%08X\n",
            checkpoints[i].name,
            pc,
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_V0],
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_V1],
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_A0],
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_A1],
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_A2],
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_A3],
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_S0],
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_S1],
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_S6],
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_S7],
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP]);
    }
}

static void be300_sync_nand_image(machine_t *m)
{
    FILE *f;

    if (!m || !m->nand_data || !m->cfg.nand_path || !m->nand.dirty)
        return;

    f = fopen(m->cfg.nand_path, "wb");
    if (!f) {
        fprintf(stderr, "[BE300] Failed to save NAND image: %s\n",
            m->cfg.nand_path);
        return;
    }
    if (fwrite(m->nand_data, 1, m->nand_size, f) != m->nand_size)
        fprintf(stderr, "[BE300] Short write while saving NAND image\n");
    fclose(f);
    m->nand.dirty = false;
    fprintf(stderr, "[BE300] NAND image saved: %s (%zu bytes)\n",
        m->cfg.nand_path, m->nand_size);
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
    gxm->prom_emulation = be300_uses_rom_boot(cfg) ? 0 : 1;
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
    cf_init(&m->cf);

    if (cfg->restore)
        m->btn_set2 = 0x80u;

    /*
     * Load kernel or NAND image.
     */
    if (cfg->kernel_path) {
        if (be300_boot_linux_path(m, cfg->kernel_path, cfg->cmdline,
                                  cfg->ram_path) != 0) {
            free(m);
            return NULL;
        }

    } else if (cfg->nand_path || cfg->restore) {
        if (cfg->nand_path) {
            if (loader_load_nand_image(m, cfg->nand_path) != 0) {
                fprintf(stderr, "[BE300] Failed to load NAND image\n");
                free(m);
                return NULL;
            }
        } else if (loader_create_blank_nand_image(m) != 0) {
            fprintf(stderr, "[BE300] Failed to create blank NAND image\n");
            free(m);
            return NULL;
        }

        if (cfg->cf_path && cf_load_image(&m->cf, cfg->cf_path) != 0) {
            fprintf(stderr, "[BE300] Failed to load CF image\n");
            free(m);
            return NULL;
        }

        if (cfg->cf_path) {
            bool nand_bootable = be300_nand_image_looks_bootable(m);
            bool cf_boot_visible = cfg->restore || !nand_bootable;

            cf_set_boot_visibility(&m->cf, cf_boot_visible);
            fprintf(stderr,
                "[BE300] CF ROM path %s (%s)\n",
                cf_boot_visible ? "enabled" : "disabled",
                cfg->restore ? "--restore" :
                (nand_bootable ? "bootable NAND present" : "NAND not bootable"));
        }

        /* True cold boot: start at ROM reset vector.
         * The ROM will read NAND, load the SPL, run the MIPS16
         * section copier and boot dispatcher, then jump to NK.exe. */
        m->cpu->pc = 0xffffffffBFC00000ULL;
        m->boot_mode = cfg->restore ? BE300_BOOT_RESTORE : BE300_BOOT_NAND;
        fprintf(stderr, "[BE300] %s: PC=0xBFC00000 (ROM reset vector)\n",
            cfg->restore ? "Restore boot" : "Cold boot");

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

                    be300_patch_boot_rom_vectors(m);
                }
        }

        fprintf(stderr, "[BE300] ROM-era TLB preloads disabled\n");

        extern void be300_register_cf_window(struct machine *, machine_t *, bool);
        be300_register_cf_window(gxm, m, cfg->log_mmio);

        /* Register VRC4173 latch (catch-all); pre-split to leave gaps for input device */
        extern void be300_register_vrc4173_latch(struct machine *, machine_t *, bool, bool);
        be300_register_vrc4173_latch(gxm, m, cfg->log_mmio, cfg->enable_ppsh);

        /* Register NAND flash; pre-split to leave gap at 0x0A00A040 for input device */
        extern void be300_register_nand(struct machine *, nand_state_t *,
            cf_state_t *, bool);
        be300_register_nand(gxm, &m->nand, &m->cf, cfg->log_mmio);

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

static uint64_t be300_sext32(uint32_t v)
{
    return (uint64_t)(int64_t)(int32_t)v;
}

static int be300_read_u32_va(machine_t *m, uint64_t va, uint32_t *value)
{
    unsigned char buf[4];

    if (!m || !m->cpu || !value)
        return 0;

    if (!m->cpu->memory_rw(m->cpu, m->cpu->mem, va, buf, sizeof(buf),
        MEM_READ, CACHE_NONE | NO_EXCEPTIONS))
        return 0;

    *value = (uint32_t)buf[0]
        | ((uint32_t)buf[1] << 8)
        | ((uint32_t)buf[2] << 16)
        | ((uint32_t)buf[3] << 24);
    return 1;
}

static uint32_t be300_read_u32_pa(machine_t *m, uint32_t pa)
{
    unsigned char buf[4];

    if (!m || !m->cpu)
        return 0;

    if (!m->cpu->memory_rw(m->cpu, m->cpu->mem, pa, buf, sizeof(buf),
        MEM_READ, PHYSICAL | NO_EXCEPTIONS | CACHE_DATA))
        return 0;

    return (uint32_t)buf[0]
        | ((uint32_t)buf[1] << 8)
        | ((uint32_t)buf[2] << 16)
        | ((uint32_t)buf[3] << 24);
}

static void be300_dump_virtual_page(machine_t *m, const char *label,
                                    uint64_t va)
{
    unsigned char page[4096];
    uint64_t paddr = 0;
    uint64_t page_va;
    uint64_t page_paddr;
    char path[128];
    FILE *f;
    size_t wrote;

    if (!m || !m->cpu || !m->cpu->translate_v2p)
        return;

    if (!m->cpu->translate_v2p(m->cpu, va, &paddr,
        FLAG_NOEXCEPTIONS | FLAG_INSTR)) {
        fprintf(stderr,
            "[BE300] STUCK_DUMP %s va=0x%08" PRIx64 " map_ok=0\n",
            label, va);
        return;
    }

    page_va = va & ~0xfffULL;
    page_paddr = paddr & ~0xfffULL;
    if (!m->cpu->memory_rw(m->cpu, m->cpu->mem, page_paddr, page,
        sizeof(page), MEM_READ,
        PHYSICAL | NO_EXCEPTIONS | CACHE_INSTRUCTION)) {
        fprintf(stderr,
            "[BE300] STUCK_DUMP %s va_page=0x%08" PRIx64
            " paddr_page=0x%08" PRIx64 " read_ok=0\n",
            label, page_va, page_paddr);
        return;
    }

    snprintf(path, sizeof(path), "stuck_%s_page_%08x.bin", label,
        (uint32_t)page_paddr);
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr,
            "[BE300] STUCK_DUMP %s va_page=0x%08" PRIx64
            " paddr_page=0x%08" PRIx64 " fopen failed for %s\n",
            label, page_va, page_paddr, path);
        return;
    }

    wrote = fwrite(page, 1, sizeof(page), f);
    fclose(f);

    fprintf(stderr,
        "[BE300] STUCK_DUMP %s va_page=0x%08" PRIx64
        " paddr_page=0x%08" PRIx64 " path=%s wrote=%zu\n",
        label, page_va, page_paddr, path, wrote);
}

static void be300_dump_virtual_dword(machine_t *m, const char *label,
                                     uint64_t va)
{
    uint32_t value = 0;
    uint64_t paddr = 0;
    int map_ok;
    int read_ok;

    if (!m || !m->cpu || !m->cpu->translate_v2p)
        return;

    map_ok = m->cpu->translate_v2p(m->cpu, va, &paddr, FLAG_NOEXCEPTIONS);
    read_ok = m->cpu->memory_rw(m->cpu, m->cpu->mem, va,
        (unsigned char *)&value, sizeof(value), MEM_READ,
        CACHE_DATA | NO_EXCEPTIONS);

    fprintf(stderr,
        "[BE300] STUCK_DWORD %s va=0x%08" PRIx64
        " map_ok=%d paddr=%s0x%08" PRIx64
        " read_ok=%d value=%s0x%08X\n",
        label,
        va,
        map_ok,
        map_ok ? "" : "(none) ",
        paddr,
        read_ok,
        read_ok ? "" : "(none) ",
        value);
}

static void be300_dump_live_nk_image(machine_t *m, uint32_t entry_va,
                                     uint32_t base_pa)
{
    uint32_t nk_pa = base_pa;
    uint32_t nk_size = 0x00800000u;
    uint32_t sig = 0;
    uint32_t p_toc = 0;
    uint32_t physfirst = 0;
    uint32_t physlast = 0;
    FILE *nk_fp;

    if (!m || !m->cpu || base_pa == 0 || base_pa >= 0x00800000u)
        return;

    nk_fp = fopen("nk_decompressed.bin", "wb");

    if (be300_read_u32_va(m,
            0xffffffffA0000000ULL | (uint64_t)(base_pa + 0x40u),
            &sig)
        && sig == 0x43454345u
        && be300_read_u32_va(m,
            0xffffffffA0000000ULL | (uint64_t)(base_pa + 0x44u),
            &p_toc)
        && be300_read_u32_va(m, be300_sext32(p_toc + 0x08u), &physfirst)
        && be300_read_u32_va(m, be300_sext32(p_toc + 0x0Cu), &physlast)
        && physlast > physfirst) {
        nk_pa = physfirst & 0x1FFFFFFFu;
        nk_size = physlast - physfirst;
    }

    fprintf(stderr,
        "[BE300] NK dump plan: entry=0x%08X base_pa=0x%08X"
        " sig=0x%08X pTOC=0x%08X physfirst=0x%08X"
        " physlast=0x%08X size=0x%08X\n",
        entry_va, base_pa, sig, p_toc, physfirst, physlast, nk_size);

    if (nk_fp) {
        uint8_t page[4096];
        uint32_t off = 0;

        while (off < nk_size) {
            uint32_t chunk = nk_size - off;
            uint64_t va;

            if (chunk > sizeof(page))
                chunk = sizeof(page);
            va = 0xffffffffA0000000ULL | (uint64_t)(nk_pa + off);
            if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                    va, page, chunk, MEM_READ, CACHE_NONE))
                fwrite(page, 1, chunk, nk_fp);
            else
                break;
            off += chunk;
        }
        fclose(nk_fp);
        fprintf(stderr,
            "[BE300] Dumped NK.exe (%u bytes) to nk_decompressed.bin\n",
            off);
    }

    {
        uint32_t sigw = 0;
        uint64_t sig_va = 0xffffffffA0000000ULL | (uint64_t)(nk_pa + 0x40u);

        if (be300_read_u32_va(m, sig_va, &sigw)) {
            fprintf(stderr,
                "[BE300] NK.exe ECEC check: 0x%08X %s\n",
                sigw, sigw == 0x43454345u ? "OK" : "MISMATCH");
        }
    }
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

    host_io_reset_stdin_state();
    host_io_set_serial_sink(be300_serial_sink, m);
    host_io_set_stdout_enabled(m->mirror_serial_to_stdout);
    host_io_set_console_stdin_enabled(!m->cfg.enable_ppsh);

    if (m->cfg.enable_ppsh) {
        fprintf(stderr,
            "[PPSH] console bridge enabled"
            " (stdin is line-buffered unless the caller supplies raw input)\n");
    }
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
    host_io_set_console_stdin_enabled(true);

    emul_executing = false;
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    cpu_run_deinit(m->gxe_machine);
    m->runtime_stopped = true;
    m->runtime_finalized = true;

    wince_boot_log_summary(m);
    fprintf(stderr, "[BE300] Emulation stopped after %" PRIi64 " instructions\n",
            m->cpu->ninstrs);
}

static bool be300_run_batch(machine_t *m)
{
    static uint64_t last_progress_pc = 0;
    static int same_progress_pc_reports = 0;
    static int stuck_dumped = 0;

    struct machine *gxm = m->gxe_machine;

    __sync_synchronize();
    if (emul_shutdown) {
        fprintf(stderr, "[BE300] Loop exit: emul_shutdown is true\n");
        wince_boot_pc_ring_dump(m);
        return false;
    }

    if (m->wince.cold_boot_copy_done && m->nand_data) {
        uint32_t raw_pc = (uint32_t)m->cpu->pc;
        uint32_t canon_pc = be300_canonicalize_nk_pc(raw_pc);

        if (m->wince.section3_step_trace_pending
            && !m->wince.section3_step_trace_active) {
            single_step = true;
            m->wince.section3_step_trace_pending = false;
            m->wince.section3_step_trace_active = true;
            fprintf(stderr,
                "[WINCE_SEC3_STEP] start rem=%u PC=0x%08X Canon=0x%08X"
                " RA=0x%08X SP=0x%08X sec3=0x%08X page0=0x%08X\n",
                (unsigned)m->wince.section3_step_trace_remaining,
                raw_pc,
                canon_pc,
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                be300_read_u32_pa(m, 0x18CCu),
                be300_read_u32_pa(m, 0x00FE5000u));
        }

        if (m->wince.type4_step_trace_pending
            && !m->wince.type4_step_trace_active) {
            single_step = true;
            m->wince.type4_step_trace_pending = false;
            m->wince.type4_step_trace_active = true;
            fprintf(stderr,
                "[WINCE_TYPE4_STEP] start rem=%u PC=0x%08X Canon=0x%08X"
                " RA=0x%08X SP=0x%08X sec3=0x%08X"
                " wrap08=0x%08X wrap14=0x%08X wrap18=0x%08X"
                " obj8c=0x%08X obj90=0x%08X\n",
                (unsigned)m->wince.type4_step_trace_remaining,
                raw_pc,
                canon_pc,
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                be300_read_u32_pa(m, 0x18CCu),
                be300_read_u32_pa(m, 0x00FE9CE4u),
                be300_read_u32_pa(m, 0x00FE9CF0u),
                be300_read_u32_pa(m, 0x00FE9CF4u),
                be300_read_u32_pa(m, 0x00FE9E30u),
                be300_read_u32_pa(m, 0x00FE9E34u));
        }

        if (!m->wince.nk_step_trace_done
            && !m->wince.nk_step_trace_active
            && canon_pc >= 0x80079400u && canon_pc < 0x80079800u) {
            single_step = true;
            m->wince.nk_step_trace_active = true;
            m->wince.nk_step_trace_remaining = 256;
            fprintf(stderr,
                "[WINCE_STEP] start PC=0x%08X Canon=0x%08X"
                " RA=0x%08X SP=0x%08X Status=0x%08X\n",
                raw_pc,
                canon_pc,
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS]);
        }

        if (m->wince.nk_step_trace_active) {
            fprintf(stderr,
                "[WINCE_STEP] pre rem=%u PC=0x%08X Canon=0x%08X"
                " RA=0x%08X SP=0x%08X Status=0x%08X EPC=0x%08X\n",
                (unsigned)m->wince.nk_step_trace_remaining,
                raw_pc,
                canon_pc,
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS],
                (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC]);
        }

        if (m->wince.section3_step_trace_active) {
            fprintf(stderr,
                "[WINCE_SEC3_STEP] pre rem=%u PC=0x%08X Canon=0x%08X"
                " RA=0x%08X SP=0x%08X sec3=0x%08X page0=0x%08X"
                " p694=0x%08X p7e4=0x%08X\n",
                (unsigned)m->wince.section3_step_trace_remaining,
                raw_pc,
                canon_pc,
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                be300_read_u32_pa(m, 0x18CCu),
                be300_read_u32_pa(m, 0x00FE5000u),
                be300_read_u32_pa(m, 0x00FE5694u),
                be300_read_u32_pa(m, 0x00FE57E4u));
        }

        if (m->wince.type4_step_trace_active) {
            fprintf(stderr,
                "[WINCE_TYPE4_STEP] pre rem=%u PC=0x%08X Canon=0x%08X"
                " RA=0x%08X SP=0x%08X sec3=0x%08X"
                " wrap08=0x%08X wrap14=0x%08X wrap18=0x%08X"
                " obj8c=0x%08X obj90=0x%08X\n",
                (unsigned)m->wince.type4_step_trace_remaining,
                raw_pc,
                canon_pc,
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                be300_read_u32_pa(m, 0x18CCu),
                be300_read_u32_pa(m, 0x00FE9CE4u),
                be300_read_u32_pa(m, 0x00FE9CF0u),
                be300_read_u32_pa(m, 0x00FE9CF4u),
                be300_read_u32_pa(m, 0x00FE9E30u),
                be300_read_u32_pa(m, 0x00FE9E34u));
        }

        {
            if (m->boot_mode == BE300_BOOT_RESTORE
                && !be300_restore_main_trace_done
                && !be300_restore_main_trace_active
                && canon_pc >= 0x80E0331Cu
                && canon_pc < 0x80E04400u) {
                single_step = true;
                be300_restore_main_trace_active = true;
                be300_restore_main_trace_remaining = 1024;
                fprintf(stderr,
                    "[RESTORE_STEP] start PC=0x%08X RA=0x%08X SP=0x%08X"
                    " v0=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X\n",
                    canon_pc,
                    (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
                    (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                    (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_V0],
                    (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_A0],
                    (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_A1],
                    (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_A2],
                    (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_A3]);
            }
        }
    }

    be300_trace_restore_format(m);
    be300_trace_restore_checkpoints(m);

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
            wince_boot_note_usermode_entry(m);
        }
    }

    /* NK.exe PC tracking after entry detected */
    if (m->wince.cold_boot_copy_done && m->nand_data) {
        static int nk_batch_log_count = 0;
        if (nk_batch_log_count < 500 && (nk_batch_log_count < 5 || nk_batch_log_count % 50 == 0
            || (((uint32_t)m->cpu->pc & 0x1FFFFFFFu) >= 0x60000u
                && ((uint32_t)m->cpu->pc & 0x1FFFFFFFu) < 0x100000u))) {
            fprintf(stderr, "[NK_PC] batch=%d PC=0x%08X Status=0x%08X"
                " SP=0x%08X RA=0x%08X\n",
                m->loop_count, (uint32_t)m->cpu->pc,
                (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA]);
            nk_batch_log_count++;
        }
    }

    /* NK.exe PC milestone tracking */
    if (m->wince.cold_boot_copy_done && m->nand_data) {
        static uint32_t milestones_seen = 0;
        uint32_t pc32 = (uint32_t)m->cpu->pc;
        uint32_t pa = pc32 & 0x1FFFFFFFu;
        struct { uint32_t va; uint32_t bit; const char *name; } checks[] = {
            { 0x80077820u, 0x01, "FUN_80077820-boot-dispatch" },
            { 0x8007B398u, 0x02, "cold-start-kernel-entry" },
            { 0x800792ACu, 0x04, "FUN_800792AC-state-save" },
            { 0x80079634u, 0x08, "warm-path-entry" },
            { 0x80079730u, 0x10, "warm-path-gpr-restore" },
            { 0x800797DCu, 0x20, "warm-path-crash-point" },
            { 0x800794C8u, 0x40, "cold-boot-continuation" },
            { 0x800795B4u, 0x80, "warm-resume-entry" },
        };
        for (int i = 0; i < 8; i++) {
            if (pc32 == checks[i].va && !(milestones_seen & checks[i].bit)) {
                milestones_seen |= checks[i].bit;
                fprintf(stderr,
                    "[NK_MILESTONE] %s PC=0x%08X batch=%d"
                    " Status=0x%08X SP=0x%08X RA=0x%08X\n",
                    checks[i].name, pc32, m->loop_count,
                    (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS],
                    (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                    (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA]);
            }
        }
    }

    /*
     * Poll PA 0x24FC for NK.exe entry point (set by SPL after decompression).
     * When detected, dump boot state BEFORE NK.exe starts executing.
     */
    if (!m->wince.cold_boot_copy_done && m->nand_data) {
        static int entry_poll_logged = 0;
        uint8_t buf24fc[4];
        uint64_t va24fc = 0xffffffffA00024FCULL;
        if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                va24fc, buf24fc, 4, MEM_READ, CACHE_NONE)) {
            uint32_t entry = buf24fc[0] | (buf24fc[1]<<8)
                | (buf24fc[2]<<16) | (buf24fc[3]<<24);
            uint32_t entry_pa = entry & 0x1FFFFFFFu;
            if (entry_pa >= 0x60000u && entry_pa < 0x100000u
                && !entry_poll_logged) {
                uint32_t base_pa = entry_pa - 4u;

                entry_poll_logged = 1;
                m->wince.cold_boot_copy_done = true;
                m->wince.cold_boot_redirected = true;
                wince_boot_pc_ring_activate(m);
                fprintf(stderr,
                    "[BE300] *** NK.exe entry at PA_24FC=0x%08X"
                    " (PC=0x%08X batch=%d) ***\n",
                    entry, (uint32_t)m->cpu->pc, m->loop_count);
                be300_dump_live_nk_image(m, entry, base_pa);
                /* Dump boot signature PA 0x2700 */
                {
                    uint8_t sig[16];
                    uint64_t sig_va = 0xffffffffA0002700ULL;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            sig_va, sig, 16, MEM_READ, CACHE_NONE)) {
                        fprintf(stderr,
                            "[BE300]   boot_sig PA 0x2700:"
                            " %02X %02X %02X %02X %02X %02X %02X %02X"
                            " %02X %02X %02X %02X %02X %02X %02X %02X\n",
                            sig[0],sig[1],sig[2],sig[3],
                            sig[4],sig[5],sig[6],sig[7],
                            sig[8],sig[9],sig[10],sig[11],
                            sig[12],sig[13],sig[14],sig[15]);
                    }
                }
                /* Dump resume_ctx PA 0x2200 */
                {
                    uint8_t ctx[32];
                    uint64_t ctx_va = 0xffffffffA0002200ULL;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            ctx_va, ctx, 32, MEM_READ, CACHE_NONE)) {
                        fprintf(stderr,
                            "[BE300]   resume_ctx PA 0x2200:"
                            " %02X%02X%02X%02X %02X%02X%02X%02X"
                            " %02X%02X%02X%02X %02X%02X%02X%02X\n",
                            ctx[0],ctx[1],ctx[2],ctx[3],
                            ctx[4],ctx[5],ctx[6],ctx[7],
                            ctx[8],ctx[9],ctx[10],ctx[11],
                            ctx[12],ctx[13],ctx[14],ctx[15]);
                    }
                }
                /* Dump PA 0x2518 (checked by FUN_80079488) */
                {
                    uint8_t f[4];
                    uint64_t f_va = 0xffffffffA0002518ULL;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            f_va, f, 4, MEM_READ, CACHE_NONE)) {
                        uint32_t val = f[0]|(f[1]<<8)|(f[2]<<16)|(f[3]<<24);
                        fprintf(stderr,
                            "[BE300]   PA 0x2518=0x%08X\n", val);
                    }
                }

                /* NOTE: Method 113 of API set 0 ("Win32") at function table
                 * 0x800753D8+0x1C4 = 0xFFFFFFFF (PSL server redirect).
                 * This is likely CreateFileW. When filesys.exe calls it
                 * during its own init, the PSL dispatch should detect
                 * that the caller IS the server and handle it locally.
                 * Patching 0xFFFFFFFF to a return-error stub doesn't work
                 * because filesys.exe needs CreateFileW to succeed. */
            }
        }
    }

    /* Detect SPL entry: PC in range 0x80F00000-0x80F0FFFF (PA 0xF00000) */
    {
        static int spl_entry_logged = 0;
        static int spl_probe_done = 0;
        uint32_t pc = (uint32_t)m->cpu->pc;
        uint32_t pa = pc & 0x1FFFFFFFu;
        /* Also detect exceptions (BEV handler entry) */
        if (pa == 0x1FC00200u || pa == 0x1FC00280u || pa == 0x1FC00380u) {
            static int exc_logged = 0;
            if (exc_logged < 5) {
                uint64_t *cp0 = m->cpu->cd.mips.coproc[0]->reg;
                fprintf(stderr,
                    "[BE300] *** BEV EXCEPTION at PA=0x%08X EPC=0x%08X Cause=0x%08X ***\n",
                    pa, (uint32_t)cp0[COP0_EPC], (uint32_t)cp0[COP0_CAUSE]);
                exc_logged++;
            }
        }
        if (!spl_entry_logged && pa >= 0xF00000u && pa < 0x1000000u) {
            fprintf(stderr,
                    "[BE300] *** SPL ENTRY DETECTED: PC=0x%08X PA=0x%08X batch=%d ***\n",
                    pc, pa, m->loop_count);
            spl_entry_logged = 1;
        }
        /* Periodically probe boot state */
        if (m->loop_count > 0 && (m->loop_count == 1 || (m->loop_count % 50000 == 0 && m->loop_count <= 200000))) {
            /* Read descriptor counter at PA 0x10034 */
            uint8_t cbuf[1];
            uint64_t cva = 0xffffffff80010034ULL;
            if (m->cpu->memory_rw(m->cpu, m->cpu->mem, cva, cbuf, 1, MEM_READ, CACHE_DATA)) {
                fprintf(stderr, "[BE300] DESC_COUNTER PA=0x10034: %d batch=%d\n",
                        cbuf[0], m->loop_count);
            }
        }
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
            /* Also probe the entry point at 0x80F00004 and JR target 0x80F02404 */
            spl_va = 0xffffffff80F00004ULL;
            if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                    spl_va, buf, 4, MEM_READ, CACHE_DATA)) {
                uint32_t instr = buf[0] | (buf[1]<<8) | (buf[2]<<16) | (buf[3]<<24);
                fprintf(stderr, "[BE300] SPL PROBE PA=0xF00004: instruction=0x%08X\n", instr);
            }
            /* Probe 0x80F02404 (SPL main code) */
            {
                uint8_t spl_code[16];
                spl_va = 0xffffffff80F02404ULL;
                if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                        spl_va, spl_code, 16, MEM_READ, CACHE_DATA)) {
                    fprintf(stderr,
                            "[BE300] SPL PROBE PA=0xF02404: "
                            "%02X%02X%02X%02X %02X%02X%02X%02X "
                            "%02X%02X%02X%02X %02X%02X%02X%02X\n",
                            spl_code[0],spl_code[1],spl_code[2],spl_code[3],
                            spl_code[4],spl_code[5],spl_code[6],spl_code[7],
                            spl_code[8],spl_code[9],spl_code[10],spl_code[11],
                            spl_code[12],spl_code[13],spl_code[14],spl_code[15]);
                }
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
     * Detect NK.exe entry and dump the decompressed image for analysis.
     * Derive the image base from the live entry at PA 0x24FC instead of
     * assuming the WinCE 3.0 layout. The .NET image enters at 0xA0029004,
     * while the 3.0 image enters at 0xA0060004.
     */
    if (!m->wince.cold_boot_copy_done && m->nand_data) {
        uint32_t pc = (uint32_t)m->cpu->pc;
        uint32_t pa = pc & 0x1FFFFFFFu;
        uint32_t entry_va = 0;
        uint32_t entry_pa = 0;
        uint32_t base_pa = 0;

        if (be300_read_u32_va(m, 0xffffffffA00024FCULL, &entry_va)
            && ((entry_va & 0xE0000000u) == 0x80000000u
                || (entry_va & 0xE0000000u) == 0xA0000000u)
            && (entry_va & 0x1FFFFFFFu) >= 4u) {
            entry_pa = entry_va & 0x1FFFFFFFu;
            base_pa = entry_pa - 4u;
        }

        if (base_pa != 0
            && base_pa < 0x00800000u
            && pa >= base_pa
            && pa < base_pa + 0x00800000u) {
            m->wince.cold_boot_copy_done = true;
            m->wince.cold_boot_redirected = true;
            fprintf(stderr,
                "[BE300] *** NK.exe entry detected at PC=0x%08X batch=%d ***\n",
                pc, m->loop_count);
            be300_dump_live_nk_image(m, entry_va, base_pa);
        }
    }

    if (!machine_run(gxm)) {
        if (m->wince.nk_step_trace_active) {
            single_step = false;
            m->wince.nk_step_trace_active = false;
            m->wince.nk_step_trace_done = true;
        }
        wince_boot_note_fatal_stop(m, "machine-no-longer-running");
        fprintf(stderr, "[BE300] Loop exit: machine no longer running"
            " (mips16=%d halted=%d PC=0x%08X)\n",
            m->cpu->cd.mips.mips16,
            m->cpu->is_halted,
            (uint32_t)m->cpu->pc);

        /* Dump PC ring buffer on crash */
        wince_boot_pc_ring_dump(m);

        /* Dump boot signature at PA 0x2700 (FUN_80079E48 cold/warm check) */
        if (m->nand_data) {
            uint8_t sig[16];
            uint64_t sig_va = 0xffffffffA0002700ULL;
            if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                    sig_va, sig, 16, MEM_READ, CACHE_NONE)) {
                fprintf(stderr,
                    "[CRASH] PA 0x2700 boot_sig:"
                    " %02X %02X %02X %02X %02X %02X %02X %02X"
                    " %02X %02X %02X %02X %02X %02X %02X %02X\n",
                    sig[0],sig[1],sig[2],sig[3],sig[4],sig[5],sig[6],sig[7],
                    sig[8],sig[9],sig[10],sig[11],sig[12],sig[13],sig[14],sig[15]);
            }
        }

        /* Basic crash diagnostics */
        {
            uint32_t pc = (uint32_t)m->cpu->pc;
            fprintf(stderr, "[CRASH] PC=0x%08X batch=%d instrs=%" PRIi64 "\n",
                pc, m->loop_count, m->cpu->ninstrs);
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

    if (m->wince.nk_step_trace_active) {
        uint32_t raw_pc = (uint32_t)m->cpu->pc;
        uint32_t canon_pc = be300_canonicalize_nk_pc(raw_pc);

        fprintf(stderr,
            "[WINCE_STEP] post rem=%u PC=0x%08X Canon=0x%08X"
            " RA=0x%08X SP=0x%08X Status=0x%08X EPC=0x%08X\n",
            (unsigned)m->wince.nk_step_trace_remaining,
            raw_pc,
            canon_pc,
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
            (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS],
            (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC]);

        if (m->wince.nk_step_trace_remaining > 0)
            m->wince.nk_step_trace_remaining--;

        if (m->wince.nk_step_trace_remaining == 0
            || canon_pc < 0x80079000u
            || canon_pc >= 0x80079880u) {
            single_step = false;
            m->wince.nk_step_trace_active = false;
            m->wince.nk_step_trace_done = true;
            fprintf(stderr,
                "[WINCE_STEP] stop PC=0x%08X Canon=0x%08X"
                " RA=0x%08X SP=0x%08X Status=0x%08X\n",
                raw_pc,
                canon_pc,
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS]);
        }
    }

    if (m->wince.section3_step_trace_active) {
        uint32_t raw_pc = (uint32_t)m->cpu->pc;
        uint32_t canon_pc = be300_canonicalize_nk_pc(raw_pc);

        fprintf(stderr,
            "[WINCE_SEC3_STEP] post rem=%u PC=0x%08X Canon=0x%08X"
            " RA=0x%08X SP=0x%08X sec3=0x%08X page0=0x%08X"
            " p694=0x%08X p7e4=0x%08X\n",
            (unsigned)m->wince.section3_step_trace_remaining,
            raw_pc,
            canon_pc,
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
            be300_read_u32_pa(m, 0x18CCu),
            be300_read_u32_pa(m, 0x00FE5000u),
            be300_read_u32_pa(m, 0x00FE5694u),
            be300_read_u32_pa(m, 0x00FE57E4u));

        if (m->wince.section3_step_trace_remaining > 0)
            m->wince.section3_step_trace_remaining--;

        if (m->wince.section3_step_trace_remaining == 0
            || canon_pc < 0x80000000u
            || canon_pc >= 0x800A8000u
            || be300_read_u32_pa(m, 0x18CCu) != UINT32_C(0x80FE5000)) {
            single_step = false;
            m->wince.section3_step_trace_active = false;
            m->wince.section3_step_trace_done = true;
            fprintf(stderr,
                "[WINCE_SEC3_STEP] stop PC=0x%08X Canon=0x%08X"
                " RA=0x%08X SP=0x%08X sec3=0x%08X page0=0x%08X\n",
                raw_pc,
                canon_pc,
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                be300_read_u32_pa(m, 0x18CCu),
                be300_read_u32_pa(m, 0x00FE5000u));
        }
    }

    {
        uint32_t raw_pc = (uint32_t)m->cpu->pc;
        uint32_t canon_pc = be300_canonicalize_nk_pc(raw_pc);

        if (m->boot_mode == BE300_BOOT_RESTORE
            && be300_restore_main_trace_active) {
            fprintf(stderr,
                "[RESTORE_STEP] post rem=%u PC=0x%08X RA=0x%08X SP=0x%08X"
                " v0=0x%08X v1=0x%08X a0=0x%08X a1=0x%08X"
                " a2=0x%08X a3=0x%08X\n",
                be300_restore_main_trace_remaining,
                canon_pc,
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_V0],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_V1],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_A0],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_A1],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_A2],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_A3]);

            if (be300_restore_main_trace_remaining > 0)
                be300_restore_main_trace_remaining--;

            if (be300_restore_main_trace_remaining == 0
                || canon_pc < 0x80E0331Cu
                || canon_pc >= 0x80E04400u) {
                single_step = false;
                be300_restore_main_trace_active = false;
                be300_restore_main_trace_done = true;
                fprintf(stderr,
                    "[RESTORE_STEP] stop PC=0x%08X RA=0x%08X SP=0x%08X"
                    " v0=0x%08X\n",
                    canon_pc,
                    (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
                    (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                    (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_V0]);
            }
        }
    }

    if (m->wince.type4_step_trace_active) {
        uint32_t raw_pc = (uint32_t)m->cpu->pc;
        uint32_t canon_pc = be300_canonicalize_nk_pc(raw_pc);

        fprintf(stderr,
            "[WINCE_TYPE4_STEP] post rem=%u PC=0x%08X Canon=0x%08X"
            " RA=0x%08X SP=0x%08X sec3=0x%08X"
            " wrap08=0x%08X wrap14=0x%08X wrap18=0x%08X"
            " obj8c=0x%08X obj90=0x%08X\n",
            (unsigned)m->wince.type4_step_trace_remaining,
            raw_pc,
            canon_pc,
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
            be300_read_u32_pa(m, 0x18CCu),
            be300_read_u32_pa(m, 0x00FE9CE4u),
            be300_read_u32_pa(m, 0x00FE9CF0u),
            be300_read_u32_pa(m, 0x00FE9CF4u),
            be300_read_u32_pa(m, 0x00FE9E30u),
            be300_read_u32_pa(m, 0x00FE9E34u));

        if (m->wince.type4_step_trace_remaining > 0)
            m->wince.type4_step_trace_remaining--;

        if (m->wince.type4_step_trace_remaining == 0
            || canon_pc < 0x80000000u
            || canon_pc >= 0x800A8000u) {
            single_step = false;
            m->wince.type4_step_trace_active = false;
            m->wince.type4_step_trace_done = true;
            fprintf(stderr,
                "[WINCE_TYPE4_STEP] stop PC=0x%08X Canon=0x%08X"
                " RA=0x%08X SP=0x%08X sec3=0x%08X"
                " wrap08=0x%08X wrap14=0x%08X wrap18=0x%08X"
                " obj8c=0x%08X obj90=0x%08X\n",
                raw_pc,
                canon_pc,
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                be300_read_u32_pa(m, 0x18CCu),
                be300_read_u32_pa(m, 0x00FE9CE4u),
                be300_read_u32_pa(m, 0x00FE9CF0u),
                be300_read_u32_pa(m, 0x00FE9CF4u),
                be300_read_u32_pa(m, 0x00FE9E30u),
                be300_read_u32_pa(m, 0x00FE9E34u));
        }
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
    be300_ppsh_poll_host_input(m);
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

        if (m->cpu->pc == last_progress_pc)
            same_progress_pc_reports++;
        else
            same_progress_pc_reports = 0;
        last_progress_pc = m->cpu->pc;

        if (!stuck_dumped &&
            same_progress_pc_reports >= 5) {
            uint32_t io_base = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_V1];
            uint32_t epc = (uint32_t)cp0[COP0_EPC];
            uint32_t badvaddr = (uint32_t)cp0[COP0_BADVADDR];
            uint32_t latch_val = 0;

            stuck_dumped = 1;
            fprintf(stderr,
                "[BE300] STUCK_PC pc=0x%08" PRIx64 " same_reports=%d"
                " status=0x%08X cause=0x%08X epc=0x%08X"
                " badvaddr=0x%08X sp=0x%08X ra=0x%08X"
                " v0=0x%08X v1=0x%08X a0=0x%08X a1=0x%08X"
                " a2=0x%08X a3=0x%08X s0=0x%08X s1=0x%08X"
                " s2=0x%08X s3=0x%08X gp=0x%08X t9=0x%08X\n",
                m->cpu->pc,
                same_progress_pc_reports,
                (uint32_t)cp0[COP0_STATUS],
                (uint32_t)cp0[COP0_CAUSE],
                (uint32_t)cp0[COP0_EPC],
                (uint32_t)cp0[COP0_BADVADDR],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_V0],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_V1],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_A0],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_A1],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_A2],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_A3],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_S0],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_S1],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_S2],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_S3],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_GP],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_T9]);
            be300_dump_virtual_page(m, "pc", m->cpu->pc);
            be300_dump_virtual_dword(m, "pc_word", m->cpu->pc);
            be300_dump_virtual_page(m, "ra",
                m->cpu->cd.mips.gpr[MIPS_GPR_RA]);
            if (epc != 0) {
                be300_dump_virtual_page(m, "epc", epc);
                be300_dump_virtual_dword(m, "epc_word", epc);
            }
            if (badvaddr != 0) {
                be300_dump_virtual_dword(m, "badvaddr", badvaddr);
                if ((badvaddr & ~0xfffU) != (epc & ~0xfffU))
                    be300_dump_virtual_page(m, "badvaddr", badvaddr);
            }

            if (m->cpu->pc < 0x80000000ULL) {
                be300_dump_virtual_dword(m, "global_01f620c8", 0x01f620c8u);
                be300_dump_virtual_dword(m, "io_base",
                    (uint64_t)io_base);
                be300_dump_virtual_dword(m, "io_03c0",
                    (uint64_t)io_base + 0x3c0u);
                be300_dump_virtual_dword(m, "io_03c4",
                    (uint64_t)io_base + 0x3c4u);
                be300_dump_virtual_dword(m, "io_03f4",
                    (uint64_t)io_base + 0x3f4u);
                if (be300_vrc4173_latch_read_u32(0x0A0003C0u, &latch_val))
                    fprintf(stderr,
                        "[BE300] STUCK_LATCH pa=0x0A0003C0 value=0x%08X\n",
                        latch_val);
                if (be300_vrc4173_latch_read_u32(0x0A0003C4u, &latch_val))
                    fprintf(stderr,
                        "[BE300] STUCK_LATCH pa=0x0A0003C4 value=0x%08X\n",
                        latch_val);
                if (be300_vrc4173_latch_read_u32(0x0A0003C8u, &latch_val))
                    fprintf(stderr,
                        "[BE300] STUCK_LATCH pa=0x0A0003C8 value=0x%08X\n",
                        latch_val);
                if (be300_vrc4173_latch_read_u32(0x0A0003F4u, &latch_val))
                    fprintf(stderr,
                        "[BE300] STUCK_LATCH pa=0x0A0003F4 value=0x%08X\n",
                        latch_val);
            }
        }

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

        /* Check timer threshold and non-XIP modules */
        if (m->wince.cold_boot_copy_done && m->nand_data) {
            static int dll_check_count = 0;
            /* Probe debugger event and timer threshold */
            {
                static int dbg_probed = 0;
                if (!dbg_probed) {
                    /* Read KData+0x2C0 (current thread ptr) */
                    uint8_t db[4];
                    uint64_t dva;
                    uint32_t cur_thd = 0, cur_prc = 0, dbg_evt = 0;
                    dva = 0xFFFFFFFFFFFFDAC0ULL;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem, dva, db, 4,
                            MEM_READ, CACHE_DATA | NO_EXCEPTIONS))
                        cur_thd = db[0]|(db[1]<<8)|(db[2]<<16)|(db[3]<<24);
                    dva = 0xFFFFFFFFFFFFDAC4ULL;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem, dva, db, 4,
                            MEM_READ, CACHE_DATA | NO_EXCEPTIONS))
                        cur_prc = db[0]|(db[1]<<8)|(db[2]<<16)|(db[3]<<24);
                    /* Read *(cur_thd + 0x24) — debugger event */
                    if (cur_thd != 0) {
                        dva = 0xFFFFFFFF00000000ULL | ((uint64_t)cur_thd + 0x24);
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem, dva, db, 4,
                                MEM_READ, CACHE_DATA | NO_EXCEPTIONS))
                            dbg_evt = db[0]|(db[1]<<8)|(db[2]<<16)|(db[3]<<24);
                    }
                    /* Also read the other blocking condition fields */
                    uint32_t prc_20 = 0, prc_0c = 0, evt_0c = 0;
                    if (cur_prc != 0) {
                        dva = 0xFFFFFFFF00000000ULL | ((uint64_t)cur_prc + 0x20);
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem, dva, db, 4,
                                MEM_READ, CACHE_DATA | NO_EXCEPTIONS))
                            prc_20 = db[0]|(db[1]<<8)|(db[2]<<16)|(db[3]<<24);
                        dva = 0xFFFFFFFF00000000ULL | ((uint64_t)cur_prc + 0x0c);
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem, dva, db, 4,
                                MEM_READ, CACHE_DATA | NO_EXCEPTIONS))
                            prc_0c = db[0]|(db[1]<<8)|(db[2]<<16)|(db[3]<<24);
                    }
                    if (dbg_evt != 0) {
                        dva = 0xFFFFFFFF00000000ULL | ((uint64_t)dbg_evt + 0x0c);
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem, dva, db, 4,
                                MEM_READ, CACHE_DATA | NO_EXCEPTIONS))
                            evt_0c = db[0]|(db[1]<<8)|(db[2]<<16)|(db[3]<<24);
                    }
                    fprintf(stderr,
                        "[DBG_PROBE] cur_thd=0x%08X cur_prc=0x%08X"
                        " *(thd+0x24)=0x%08X\n"
                        "  prc+0x20=0x%08X (>>25=0x%02X)"
                        " prc+0x0c=0x%08X (>>25=0x%02X)"
                        " evt+0x0c=0x%08X\n"
                        "  BLOCK=%s\n",
                        cur_thd, cur_prc, dbg_evt,
                        prc_20, prc_20 >> 25,
                        prc_0c, prc_0c >> 25,
                        evt_0c,
                        (dbg_evt != 0 &&
                         (prc_20 >> 25) == (prc_0c >> 25) &&
                         evt_0c != 0) ? "YES" : "NO");
                    dbg_probed = 1;
                }
            }
            /* Probe critical section at 0x80669740 — check on every report */
            {
                static int cs_probed = 0;
                static uint32_t prev_count = 0;
                if (cs_probed < 10) {
                    uint8_t cb[8];
                    uint64_t cva = 0xffffffff80669740ULL;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem, cva, cb, 8,
                            MEM_READ, CACHE_DATA | NO_EXCEPTIONS)) {
                        uint32_t lock_count = cb[0]|(cb[1]<<8)|(cb[2]<<16)|(cb[3]<<24);
                        uint32_t owner = cb[4]|(cb[5]<<8)|(cb[6]<<16)|(cb[7]<<24);
                        if (lock_count != 0 || cs_probed == 0) {
                            fprintf(stderr,
                                "[CS_PROBE] 0x80669740: count=%u owner=0x%08X\n",
                                lock_count, owner);
                        }
                    }
                    cs_probed++;
                }
            }
            /* Also probe module table entry for index 7 */
            {
                static int mt_probed = 0;
                if (!mt_probed) {
                    uint8_t mb[20];
                    uint64_t mva = 0xffffffff8066014CULL;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem, mva, mb, 20,
                            MEM_READ, CACHE_DATA | NO_EXCEPTIONS)) {
                        uint32_t w[5];
                        for (int j = 0; j < 5; j++)
                            w[j] = mb[j*4]|(mb[j*4+1]<<8)|(mb[j*4+2]<<16)|(mb[j*4+3]<<24);
                        fprintf(stderr,
                            "[MOD_TABLE] idx7 at 0x8066014C:"
                            " %08X %08X %08X %08X %08X\n",
                            w[0], w[1], w[2], w[3], w[4]);
                    }
                    mt_probed = 1;
                }
            }
            /* Probe sleep queue head and timer threshold */
            {
                uint8_t qb[4];
                uint64_t qva = 0xffffffff8066961CULL;
                uint32_t qhead = 0;
                if (m->cpu->memory_rw(m->cpu, m->cpu->mem, qva, qb, 4,
                        MEM_READ, CACHE_DATA | NO_EXCEPTIONS))
                    qhead = qb[0]|(qb[1]<<8)|(qb[2]<<16)|(qb[3]<<24);
                if (qhead != 0 && dll_check_count < 5) {
                    fprintf(stderr, "[SLEEP_Q] head=0x%08X\n", qhead);
                }
            }
            /* Probe timer threshold at 0x80669654 */
            {
                uint8_t tb[4];
                uint64_t tva = 0xffffffff80669654ULL;
                if (m->cpu->memory_rw(m->cpu, m->cpu->mem, tva, tb, 4,
                        MEM_READ, CACHE_DATA | NO_EXCEPTIONS)) {
                    uint32_t thresh = tb[0]|(tb[1]<<8)|(tb[2]<<16)|(tb[3]<<24);
                    if (thresh != 0 && dll_check_count < 5) {
                        fprintf(stderr, "[TIMER_THRESH] 0x80669654=0x%08X\n", thresh);
                    }
                }
            }
            if (dll_check_count < 3) {
                dll_check_count++;
                struct { uint32_t va; const char *name; } mods[] = {
                    { 0x8079F000, "ddi.dll" },
                    { 0x807BA000, "touch.dll" },
                    { 0x807C3000, "keybddr.dll" },
                };
                uint8_t tbuf[16];
                for (int mi = 0; mi < 3; mi++) {
                    uint64_t a = 0xFFFFFFFF00000000ULL | mods[mi].va;
                    int ok = m->cpu->memory_rw(m->cpu, m->cpu->mem,
                        a, tbuf, 16, MEM_READ, CACHE_DATA | NO_EXCEPTIONS);
                    int nz = 0;
                    for (int b = 0; b < 16; b++) nz += (tbuf[b] != 0);
                    fprintf(stderr, "[MOD_CHECK] %s VA=0x%08X: %s (%02X%02X%02X%02X...)\n",
                        mods[mi].name, mods[mi].va,
                        nz ? "LOADED" : "ZERO",
                        tbuf[0], tbuf[1], tbuf[2], tbuf[3]);
                }
                /* check again next time */
                /* Probe page table for non-XIP module vbases */
                if (dll_check_count == 1) {
                    struct { uint32_t vbase; const char *name; } nxmods[] = {
                        { 0x01A50000u, "ddi.dll" },
                        { 0x01B00000u, "touch.dll" },
                    };
                    for (int ni = 0; ni < 2; ni++) {
                        uint32_t vb = nxmods[ni].vbase;
                        /* Walk page table: section → L2 → PTE
                         * Same algorithm as TLB handler at 0x8008C418 */
                        uint32_t sec_idx = (vb >> 25) & 0x3Fu;
                        uint32_t sec_va = 0xFFFFD8C0u + sec_idx * 4u;
                        uint32_t sec_val = 0;
                        uint8_t rb[4];
                        uint64_t rva = 0xFFFFFFFF00000000ULL | sec_va;
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem, rva, rb, 4,
                                MEM_READ, CACHE_DATA | NO_EXCEPTIONS))
                            sec_val = rb[0]|(rb[1]<<8)|(rb[2]<<16)|(rb[3]<<24);

                        uint32_t l2_boff = (vb >> 14) & 0x7FCu;
                        uint32_t l2_val = 0;
                        if (sec_val != 0) {
                            rva = 0xFFFFFFFF00000000ULL | (sec_val + l2_boff);
                            if (m->cpu->memory_rw(m->cpu, m->cpu->mem, rva, rb, 4,
                                    MEM_READ, CACHE_DATA | NO_EXCEPTIONS))
                                l2_val = rb[0]|(rb[1]<<8)|(rb[2]<<16)|(rb[3]<<24);
                        }

                        uint32_t lo0 = 0, lo1 = 0;
                        if ((int32_t)l2_val < 0) {
                            uint32_t pte_off = (vb >> 10) & 0x38u;
                            rva = 0xFFFFFFFF00000000ULL | (l2_val + pte_off + 12u);
                            if (m->cpu->memory_rw(m->cpu, m->cpu->mem, rva, rb, 4,
                                    MEM_READ, CACHE_DATA | NO_EXCEPTIONS))
                                lo0 = rb[0]|(rb[1]<<8)|(rb[2]<<16)|(rb[3]<<24);
                            rva = 0xFFFFFFFF00000000ULL | (l2_val + pte_off + 16u);
                            if (m->cpu->memory_rw(m->cpu, m->cpu->mem, rva, rb, 4,
                                    MEM_READ, CACHE_DATA | NO_EXCEPTIONS))
                                lo1 = rb[0]|(rb[1]<<8)|(rb[2]<<16)|(rb[3]<<24);
                        }

                        /* Decode PA from EntryLo (R4100: PFN in bits 25:6, PA = PFN << 10) */
                        uint32_t pa0 = ((lo0 >> 6) & 0xFFFFFu) << 10;
                        uint32_t pa1 = ((lo1 >> 6) & 0xFFFFFu) << 10;
                        /* Read first 4 bytes at mapped PA */
                        uint32_t data0 = 0;
                        if (pa0 != 0) {
                            unsigned char *hp = memory_paddr_to_hostaddr(
                                m->cpu->mem, pa0, MEM_READ);
                            if (hp)
                                data0 = hp[0]|(hp[1]<<8)|(hp[2]<<16)|(hp[3]<<24);
                        }
                        fprintf(stderr,
                            "[VBASE_PROBE] %s vbase=0x%08X"
                            " sec[%u]=0x%08X l2=0x%08X"
                            " lo0=0x%08X(PA=0x%08X)"
                            " lo1=0x%08X(PA=0x%08X)"
                            " data@PA=0x%08X\n",
                            nxmods[ni].name, vb,
                            sec_idx, sec_val, l2_val,
                            lo0, pa0, lo1, pa1, data0);
                        /* Compare L2 entries across a range for both tables.
                         * Dump entries around ddi.dll's L2 offset (0x694)
                         * to see which entries have demand-page markers. */
                        {
                            uint32_t orig_sec = 0x80668CC0u;
                            uint32_t proc_sec = sec_val;
                            unsigned li;
                            /* Dump 8 L2 entries around the target offset */
                            uint32_t start_off = (l2_boff >= 0x10)
                                ? (l2_boff - 0x10) : 0;
                            fprintf(stderr,
                                "[L2_COMPARE] %s shared=0x%08X"
                                " proc=0x%08X l2_boff=0x%X:\n",
                                nxmods[ni].name,
                                orig_sec, proc_sec, l2_boff);
                            for (li = 0; li < 12; li++) {
                                uint32_t off = start_off + li * 4;
                                uint32_t sv = 0, pv = 0;
                                rva = 0xFFFFFFFF00000000ULL
                                    | (orig_sec + off);
                                if (m->cpu->memory_rw(m->cpu,
                                        m->cpu->mem, rva, rb, 4,
                                        MEM_READ,
                                        CACHE_DATA | NO_EXCEPTIONS))
                                    sv = rb[0]|(rb[1]<<8)
                                        |(rb[2]<<16)|(rb[3]<<24);
                                if (proc_sec != 0) {
                                    rva = 0xFFFFFFFF00000000ULL
                                        | (proc_sec + off);
                                    if (m->cpu->memory_rw(m->cpu,
                                            m->cpu->mem, rva, rb, 4,
                                            MEM_READ,
                                            CACHE_DATA|NO_EXCEPTIONS))
                                        pv = rb[0]|(rb[1]<<8)
                                            |(rb[2]<<16)|(rb[3]<<24);
                                }
                                if (sv != pv || off == l2_boff)
                                    fprintf(stderr,
                                        "[L2_COMPARE]   +0x%03X:"
                                        " shared=0x%08X"
                                        " proc=0x%08X%s\n",
                                        off, sv, pv,
                                        (off == l2_boff)
                                        ? " <-- target" : "");
                            }
                        }
                    }
                }
            }
        }
        /* Probe WinCE tick counters to see if scheduler tick is running */
        if (m->wince.cold_boot_copy_done && m->nand_data) {
            uint8_t buf[4];
            uint64_t va;
            uint32_t tick_lo = 0, tick_hi = 0, curmsec = 0;
            /* PA 0x660030/0x660034 = VA 0x80660030 (CurMSec 64-bit) */
            va = 0xffffffff80660030ULL;
            if (m->cpu->memory_rw(m->cpu, m->cpu->mem, va, buf, 4,
                    MEM_READ, CACHE_DATA | NO_EXCEPTIONS))
                tick_lo = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
            va = 0xffffffff80660034ULL;
            if (m->cpu->memory_rw(m->cpu, m->cpu->mem, va, buf, 4,
                    MEM_READ, CACHE_DATA | NO_EXCEPTIONS))
                tick_hi = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
            /* Probe VA 0xFFFFD894/0xFFFFD898 through TLB (ISR tick counters) */
            uint32_t isr_cnt1 = 0, isr_cnt2 = 0;
            va = 0xFFFFFFFFFFFFD894ULL;
            if (m->cpu->memory_rw(m->cpu, m->cpu->mem, va, buf, 4,
                    MEM_READ, CACHE_DATA | NO_EXCEPTIONS))
                isr_cnt1 = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
            va = 0xFFFFFFFFFFFFD898ULL;
            if (m->cpu->memory_rw(m->cpu, m->cpu->mem, va, buf, 4,
                    MEM_READ, CACHE_DATA | NO_EXCEPTIONS))
                isr_cnt2 = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
            /* Dump KData at VA 0xFFFFD800 — thread/process pointers */
            {
                static int kdata_dumped = 0;
                if (!kdata_dumped) {
                    uint32_t kd[16];
                    int kok = 1;
                    for (int k = 0; k < 16; k++) {
                        uint64_t a = 0xFFFFFFFFFFFFD800ULL + (uint64_t)k * 4;
                        if (!m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                a, buf, 4,
                                MEM_READ, CACHE_DATA | NO_EXCEPTIONS)) {
                            kok = 0; break;
                        }
                        kd[k] = buf[0]|(buf[1]<<8)|
                                 (buf[2]<<16)|(buf[3]<<24);
                    }
                    if (kok) {
                        fprintf(stderr, "[KDATA]");
                        for (int k = 0; k < 16; k++)
                            fprintf(stderr, " %08X", kd[k]);
                        fprintf(stderr, "\n");
                    } else {
                        fprintf(stderr, "[KDATA] TLB miss\n");
                    }
                    kdata_dumped = 1;
                }
            }
            /* Dump wired TLB entries that map 0xFFFFDxxx */
            {
                static int tlb_dumped = 0;
                if (!tlb_dumped) {
                    struct mips_coproc *cp0 = m->cpu->cd.mips.coproc[0];
                    int wired = (int)cp0->reg[COP0_WIRED];
                    int ntlbs = cp0->nr_of_tlbs;
                    fprintf(stderr, "[TLB_DUMP] Wired=%d total=%d\n",
                        wired, ntlbs);
                    for (int i = 0; i < ntlbs && i < 8; i++) {
                        uint64_t hi = cp0->tlbs[i].hi;
                        uint64_t lo0 = cp0->tlbs[i].lo0;
                        uint64_t lo1 = cp0->tlbs[i].lo1;
                        uint64_t mask = cp0->tlbs[i].mask;
                        uint32_t vpn2 = (uint32_t)(hi >> 13) << 13;
                        uint32_t pfn0 = (uint32_t)((lo0 >> 6) << 12);
                        uint32_t pfn1 = (uint32_t)((lo1 >> 6) << 12);
                        fprintf(stderr,
                            "[TLB_DUMP] [%d] hi=0x%08X lo0=0x%08X"
                            " lo1=0x%08X mask=0x%08X"
                            " → VPN=0x%08X PFN0=0x%08X PFN1=0x%08X"
                            " V0=%d V1=%d\n",
                            i, (uint32_t)hi, (uint32_t)lo0,
                            (uint32_t)lo1, (uint32_t)mask,
                            vpn2, pfn0, pfn1,
                            (int)(lo0 & 2) >> 1, (int)(lo1 & 2) >> 1);
                    }
                    tlb_dumped = 1;
                }
            }
            /* Also read PA 0x4894/0x4898 directly via kseg0 */
            uint32_t pa_cnt1 = 0, pa_cnt2 = 0;
            va = 0xffffffff80004894ULL;
            if (m->cpu->memory_rw(m->cpu, m->cpu->mem, va, buf, 4,
                    MEM_READ, CACHE_DATA | NO_EXCEPTIONS))
                pa_cnt1 = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
            va = 0xffffffff80004898ULL;
            if (m->cpu->memory_rw(m->cpu, m->cpu->mem, va, buf, 4,
                    MEM_READ, CACHE_DATA | NO_EXCEPTIONS))
                pa_cnt2 = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
            fprintf(stderr, "[BE300]   TICK=0x%08X:%08X ISR=%u/%u PA=%u/%u\n",
                tick_hi, tick_lo, isr_cnt1, isr_cnt2, pa_cnt1, pa_cnt2);
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
        .debug_serial = false,
        .enable_ppsh = false,
        .restore = false,
        .rom_path = NULL,
        .kernel_path = NULL,
        .cmdline = NULL,
        .ram_path = NULL,
        .nand_path = NULL,
        .cf_path = NULL,
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
    be300_sync_nand_image(m);
    cf_save_image(&m->cf);

    if (m->nand_data) {
        free(m->nand_data);
        m->nand_data = NULL;
    }
    cf_destroy(&m->cf);

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
