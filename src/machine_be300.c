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
static int be300_read_u32_va(machine_t *m, uint64_t va, uint32_t *value);

static uint32_t be300_canonicalize_nk_pc(uint32_t pc)
{
    if ((pc & 0xE0000000u) == 0x80000000u
        || (pc & 0xE0000000u) == 0xA0000000u)
        return (pc & 0x1FFFFFFFu) | 0x80000000u;
    return pc;
}

static uint64_t be300_va32(uint32_t va)
{
    return 0xffffffff00000000ULL | (uint64_t)va;
}

static bool be300_env_flag_enabled(const char *name)
{
    const char *value = getenv(name);

    if (!value || value[0] == '\0')
        return false;

    if (strcmp(value, "0") == 0
        || strcmp(value, "false") == 0
        || strcmp(value, "FALSE") == 0
        || strcmp(value, "no") == 0
        || strcmp(value, "NO") == 0)
        return false;

    return true;
}

static const char *be300_addr_region_name(uint32_t addr)
{
    if (addr >= 0xBFC00000u)
        return "kseg1-rom";
    if (addr >= 0xA0F00000u && addr < 0xA1000000u)
        return "kseg1-spl";
    if (addr >= 0xA0000000u)
        return "kseg1";
    if (addr >= 0x9FC00000u && addr < 0xA0000000u)
        return "kseg0-rom";
    if (addr >= 0x80F00000u && addr < 0x81000000u)
        return "kseg0-spl";
    if (addr >= 0x80000000u)
        return "kseg0";
    return "mapped";
}

static void be300_record_early_pc(machine_t *m, uint32_t pc)
{
    if (!m || m->early_bev_trace_fired || m->wince.cold_boot_copy_done)
        return;

    m->early_pc_ring[m->early_pc_ring_next] = pc;
    m->early_pc_ring_next =
        (uint8_t)((m->early_pc_ring_next + 1u) % BE300_EARLY_PC_RING_CAP);
    if (m->early_pc_ring_count < BE300_EARLY_PC_RING_CAP)
        m->early_pc_ring_count++;
}

static void be300_dump_words_va(machine_t *m, const char *label,
                                uint32_t va, size_t n_words)
{
    if (!m || !m->cpu || !label || n_words == 0)
        return;

    fprintf(stderr,
        "[BE300][EARLY_BEV] %s base=0x%08X region=%s\n",
        label, va, be300_addr_region_name(va));

    for (size_t i = 0; i < n_words; i++) {
        uint32_t word = 0;
        uint32_t word_va = (va & ~3u) + (uint32_t)(i * 4u);

        if (be300_read_u32_va(m, be300_va32(word_va), &word)) {
            fprintf(stderr,
                "[BE300][EARLY_BEV]   0x%08X: 0x%08X\n",
                word_va, word);
        } else {
            fprintf(stderr,
                "[BE300][EARLY_BEV]   0x%08X: <unreadable>\n",
                word_va);
        }
    }
}

static void be300_dump_recent_early_pcs(machine_t *m)
{
    if (!m || m->early_pc_ring_count == 0)
        return;

    fprintf(stderr,
        "[BE300][EARLY_BEV] recent_pcs count=%u\n",
        (unsigned)m->early_pc_ring_count);

    for (uint8_t i = 0; i < m->early_pc_ring_count; i++) {
        uint8_t idx = (uint8_t)((m->early_pc_ring_next
            + BE300_EARLY_PC_RING_CAP - m->early_pc_ring_count + i)
            % BE300_EARLY_PC_RING_CAP);
        uint32_t pc = m->early_pc_ring[idx];

        fprintf(stderr,
            "[BE300][EARLY_BEV]   pc[%u]=0x%08X region=%s\n",
            (unsigned)i, pc, be300_addr_region_name(pc));
    }
}

static void be300_log_first_early_bev(machine_t *m, uint32_t vector_pa)
{
    uint64_t *cp0;
    uint32_t epc;
    uint32_t cause;
    uint32_t status;
    uint32_t badvaddr;
    uint32_t entryhi;
    uint32_t context;
    uint32_t pagemask;
    uint32_t vector_va;

    if (!m || !m->cpu || m->early_bev_trace_fired)
        return;

    cp0 = m->cpu->cd.mips.coproc[0]->reg;
    epc = (uint32_t)cp0[COP0_EPC];
    cause = (uint32_t)cp0[COP0_CAUSE];
    status = (uint32_t)cp0[COP0_STATUS];
    badvaddr = (uint32_t)cp0[COP0_BADVADDR];
    entryhi = (uint32_t)cp0[COP0_ENTRYHI];
    context = (uint32_t)cp0[COP0_CONTEXT];
    pagemask = (uint32_t)cp0[COP0_PAGEMASK];
    vector_va = 0xBFC00000u + (vector_pa - 0x1FC00000u);

    m->early_bev_trace_fired = true;

    fprintf(stderr,
        "[BE300][EARLY_BEV] vector_pa=0x%08X vector_va=0x%08X"
        " pc=0x%08X epc=0x%08X cause=0x%08X status=0x%08X"
        " badvaddr=0x%08X entryhi=0x%08X context=0x%08X"
        " pagemask=0x%08X asid=%u exl=%u erl=%u bev=%u bd=%u\n",
        vector_pa,
        vector_va,
        (uint32_t)m->cpu->pc,
        epc,
        cause,
        status,
        badvaddr,
        entryhi,
        context,
        pagemask,
        entryhi & 0xFFu,
        (status & STATUS_EXL) ? 1u : 0u,
        (status & STATUS_ERL) ? 1u : 0u,
        (status & STATUS_BEV) ? 1u : 0u,
        (cause & CAUSE_BD) ? 1u : 0u);
    fprintf(stderr,
        "[BE300][EARLY_BEV] epc_region=%s badvaddr_region=%s\n",
        be300_addr_region_name(epc & ~1u),
        be300_addr_region_name(badvaddr));

    be300_dump_recent_early_pcs(m);
    be300_dump_words_va(m, "vector_words", vector_va, 8);
    be300_dump_words_va(m, "epc_words", epc & ~1u, 8);

    if (m->stop_on_first_early_bev) {
        fprintf(stderr,
            "[BE300][EARLY_BEV] stopping after first early BEV"
            " due to BE300_STOP_ON_FIRST_EARLY_BEV\n");
        emul_shutdown = true;
        __sync_synchronize();
    }
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
    m->stop_on_first_early_bev =
        be300_env_flag_enabled("BE300_STOP_ON_FIRST_EARLY_BEV");
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
         * The image is used verbatim; if the emulator ever takes an
         * unexpected early BEV exception, the single-shot trace above
         * should capture it instead of patching the ROM in place.
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
    m->early_bev_trace_fired = false;
    m->early_pc_ring_count = 0;
    m->early_pc_ring_next = 0;
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

    if (m->stop_on_first_early_bev) {
        fprintf(stderr,
            "[BE300] Early BEV diagnostics: stop_on_first=yes\n");
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

    be300_record_early_pc(m, (uint32_t)m->cpu->pc);

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
        uint32_t pc = (uint32_t)m->cpu->pc;
        uint32_t pa = pc & 0x1FFFFFFFu;
        /* Also detect exceptions (BEV handler entry) */
        if (pa == 0x1FC00200u || pa == 0x1FC00280u || pa == 0x1FC00380u) {
            be300_log_first_early_bev(m, pa);
        }
        if (!spl_entry_logged && pa >= 0xF00000u && pa < 0x1000000u) {
            fprintf(stderr,
                    "[BE300] *** SPL ENTRY DETECTED: PC=0x%08X PA=0x%08X batch=%d ***\n",
                    pc, pa, m->loop_count);
            spl_entry_logged = 1;
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
