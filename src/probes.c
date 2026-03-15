#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "probes.h"
#include "machine.h"

/*
 * Invalid instruction hook — handles VR4131 MACC instructions
 * (opcode 0x1C / SPECIAL2) which are not part of standard MIPS32.
 */
bool insn_invalid_hook(uc_engine *uc, void *user_data)
{
    (void)user_data;
    uint64_t pc = 0;
    uc_reg_read(uc, UC_MIPS_REG_PC, &pc);

    uint32_t insn = 0;
    if (!read_insn_best_effort(uc, pc, &insn)) {
        fprintf(stderr, "[CPU] Cannot read insn at PC=0x%016" PRIX64 "\n", pc);
        return false;
    }

    /* Try MACC decode first */
    if (macc_execute(uc, insn))
        return true;

    /* Unknown instruction — log and stop */
    fprintf(stderr, "[CPU] Illegal instruction PC=0x%016" PRIX64 " insn=0x%08X\n",
            pc, insn);
    return false;
}

/* ------------------------------------------------------------------ */
/* One-shot checkpoint logging                                           */
/* ------------------------------------------------------------------ */

/*
 * Checkpoint table: VA of well-known kernel functions / branch sites.
 * This table intentionally mixes symbols from multiple known test kernels
 * (linux4be 2.6.8.1 and older 2.4.x images) so the same binary can probe
 * whichever kernel is currently booted.
 *
 * Flags:
 *   print_a0_str: read $a0 as a VA→string and print alongside the name.
 */
static const struct {
    uint32_t va;
    const char *name;
    bool print_a0_str;
} checkpoint_table[] = {
    /* ICU / timer init */
    { 0x80275b20u, "vr41xx_icu_init",                 false },
    /* High-level function entries */
    { 0x80001558u, "rest_init",                       false },
    { 0x80001580u, "do_pre_smp_initcalls",            false },
    { 0x80001770u, "init (kernel thread, 2.4)",       false },
    { 0x800015d0u, "init (kernel thread)",            false },
    { 0x80272918u, "do_basic_setup",                  false },
    { 0x802727d0u, "do_initcalls",                    false },
    /* Fine-grained probes inside init() around the sys_access branch */
    { 0x80001614u, "init: JAL sys_access(/init?)",    false },
    { 0x8000161cu, "init: BNE (sys_access result)",   false },
    { 0x80001624u, "init: B   (skip prepare_ns)",     false },
    { 0x80001630u, "init: JAL prepare_namespace",     false },
    { 0x80273470u, "prepare_namespace (entry)",       false },
    { 0x80001638u, "init: JAL free_initmem",          false },
    /* exec / page-fault path */
    { 0x80001690u, "init: JAL run_init_process [execute_command]", false },
    { 0x8000169cu, "init: JAL run_init_process [/sbin/init]",      false },
    { 0x80001598u, "run_init_process (entry)",        true  },
    { 0x80007394u, "sys_execve (entry, 2.4)",         true  },
    { 0x80040730u, "call_usermodehelper (entry)",    true  },
    { 0x80040530u, "____call_usermodehelper (entry)",  true  },
    { 0x8000dcc8u, "sys_execve (entry)",              true  },
    { 0x8004a9d0u, "do_execve (entry, 2.4)",          true  },
    { 0x8004a774u, "search_binary_handler (2.4)",     false },
    { 0x80017a40u, "panic (2.4)",                     true  },
    { 0x80080cb0u, "do_execve (entry)",               true  },
    { K24_DO_PAGE_FAULT, "do_page_fault (entry, 2.4)", false },
    { 0x80016ef0u,       "do_page_fault (entry, alt)", false },
    { 0x800a7378u, "create_elf_tables (entry)",       false },
    /* Post-inet_init initcalls (last two in table) */
    { 0x80286440u, "af_unix_init",                    false },
    { 0x802864d8u, "packet_init",                     false },
    /* Inside inet_init — call sites (JAL instructions) */
    { 0x80285f58u, "inet_init: JAL sock_register",    false },
    { 0x80286038u, "inet_init: JAL arp_init",         false },
    { 0x80286040u, "inet_init: JAL ip_init",          false },
    { 0x80286050u, "inet_init: JAL tcp_init",         false },
    /* Sub-function entries */
    { 0x80285458u, "ip_init",                         false },
    { 0x802854c0u, "tcp_init",                        false },
    { 0x80285a10u, "arp_init",                        false },
    { 0x80284fb8u, "ip_rt_init",                      false },
    { 0x80162dd0u, "ipfrag_init",                     false },
    { 0x80150378u, "neigh_table_init",                false },
    { 0x8027fd98u, "alloc_large_system_hash",         false },
    { 0x80286208u, "fib_hash_init",                   false },
    /* inet_register_protosw loop site */
    { 0x80286020u, "inet_init: JAL inet_register_protosw (loop)", false },
};
#define CHECKPOINT_COUNT ((int)(sizeof(checkpoint_table)/sizeof(checkpoint_table[0])))

static bool checkpoint_fired[CHECKPOINT_COUNT];

/*
 * Read a NUL-terminated string from guest VA into a host buffer.
 * Returns the number of bytes read (excluding NUL), or 0 on failure.
 */
int read_guest_string(uc_engine *uc, uint64_t va, char *buf, int bufsz)
{
    /* Try direct PA (kseg0/kseg1) */
    uint64_t pa = 0;
    if (va_to_pa_kseg(va, &pa)) {
        int i;
        for (i = 0; i < bufsz - 1; i++) {
            uint8_t c = 0;
            if (uc_mem_read(uc, pa + i, &c, 1) != UC_ERR_OK) break;
            buf[i] = (char)c;
            if (c == 0) { buf[i] = 0; return i; }
        }
        buf[i] = 0;
        return i;
    }
    /*
     * Fallback: try reading directly at VA.  Useful for user pointers when
     * Unicorn already has the corresponding pages mapped.
     */
    {
        int i;
        for (i = 0; i < bufsz - 1; i++) {
            uint8_t c = 0;
            if (uc_mem_read(uc, va + i, &c, 1) != UC_ERR_OK) break;
            buf[i] = (char)c;
            if (c == 0) { buf[i] = 0; return i; }
        }
        buf[i] = 0;
        if (i > 0)
            return i;
    }
    buf[0] = 0;
    return 0;
}

void checkpoint_hook(uc_engine *uc, uint64_t address,
                            uint32_t size, void *user_data)
{
    (void)size; (void)address;
    int idx = (int)(uintptr_t)user_data;
    if (checkpoint_fired[idx]) return;
    checkpoint_fired[idx] = true;

    if (checkpoint_table[idx].print_a0_str) {
        uint64_t a0 = 0;
        uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
        char str[128] = "<unreadable>";
        read_guest_string(uc, a0, str, sizeof(str));
        fprintf(stderr, "[CHECKPOINT] >>> %s  a0=0x%08" PRIX64 " \"%s\" <<<\n",
                checkpoint_table[idx].name, a0, str);
    } else {
        fprintf(stderr, "[CHECKPOINT] >>> %s <<<\n", checkpoint_table[idx].name);
    }
}

/*
 * Arm a focused late-boot trace window when init starts execing /sbin/init.
 * This gives per-batch visibility into whether uc_emu_start keeps returning
 * (slow loop) or wedges inside a single batch right after run_init_process.
 */
void run_init_entry_trace_hook(uc_engine *uc, uint64_t address,
                                      uint32_t size, void *user_data)
{
    (void)uc; (void)address; (void)size;
    machine_t *m = user_data;
    m->post_init_trace_window = true;
    m->post_init_trace_batches = 0;
    m->tlb_trace_window = true;
    fprintf(stderr, "[TLB_TRACE] armed and activated by run_init_process entry\n");
}

/*
 * do_initcalls tracer: fires at the JALR site (0x80272874) inside
 * do_initcalls for every initcall invocation.  Reads $v0 ($2) which
 * holds the function pointer just before the JALR executes.
 * Limited to 64 fires to avoid log flooding.
 */
static int initcall_trace_count = 0;
void initcall_trace_hook(uc_engine *uc, uint64_t address,
                                uint32_t size, void *user_data)
{
    (void)size; (void)address; (void)user_data;
    if (initcall_trace_count >= 64) return;
    initcall_trace_count++;
    uint64_t fn = 0;
    uc_reg_read(uc, UC_MIPS_REG_V0, &fn);
    fprintf(stderr, "[INITCALL] #%02d  fn=0x%08" PRIX64 "\n",
            initcall_trace_count, (uint64_t)(uint32_t)fn);
}

/* ------------------------------------------------------------------ */
/* RCU / wait_for_completion diagnostic probes                          */
/* ------------------------------------------------------------------ */

/*
 * Multi-fire probes for the RCU grace-period path.
 * Each fires up to RCU_PROBE_LIMIT times, printing a short line so we
 * can see whether these functions are reached while init is blocked in
 * wait_for_completion.
 *
 * Addresses for linux4be20040908/vmlinux (confirmed via nm):
 *   0x801ab590  wait_for_completion   — init should block here in synchronize_kernel
 *   0x800379a8  do_timer              — called from timer interrupt handler
 *   0x800428b8  rcu_check_callbacks   — called from scheduler_tick (via do_timer)
 *   0x80042780  rcu_process_callbacks — RCU tasklet; fires the call_rcu callbacks
 *   0x8000eb30  timer_interrupt       — top-level timer interrupt handler
 */
#define RCU_PROBE_LIMIT 8

static int rcu_probe_wait_count    = 0;
static int rcu_probe_dotimer_count = 0;
static int rcu_probe_rcucheck_count= 0;
static int rcu_probe_rcuproc_count = 0;
static int rcu_probe_timerint_count= 0;

void rcu_probe_hook(uc_engine *uc, uint64_t address,
                           uint32_t size, void *user_data)
{
    (void)uc; (void)size;
    int idx = (int)(uintptr_t)user_data;
    int *cnt;
    const char *tag;
    switch (idx) {
        case 0: cnt = &rcu_probe_wait_count;     tag = "wait_for_completion";   break;
        case 1: cnt = &rcu_probe_dotimer_count;  tag = "do_timer";              break;
        case 2: cnt = &rcu_probe_rcucheck_count; tag = "rcu_check_callbacks";   break;
        case 3: cnt = &rcu_probe_rcuproc_count;  tag = "rcu_process_callbacks"; break;
        case 4: cnt = &rcu_probe_timerint_count; tag = "timer_interrupt";       break;
        default: return;
    }
    if (*cnt >= RCU_PROBE_LIMIT) return;
    (*cnt)++;
    fprintf(stderr, "[RCU_PROBE] #%d %s  PC=0x%08" PRIX64 "\n",
            *cnt, tag, (uint64_t)(uint32_t)address);
}

/* run_init_process execve syscall site probes (2.4 kernel). */
void init_execve_site_probe_hook(uc_engine *uc, uint64_t address,
                                        uint32_t size, void *user_data)
{
    (void)size;
    machine_t *m = user_data;
    uint32_t pc = (uint32_t)address;
    int idx = -1;
    const char *site = NULL;
    switch (pc) {
    case RUN_INIT_SYSCALL_EPC:    idx = 0; site = "run_init SYSCALL"; break;
    case RUN_INIT_SYSCALL_RET_PC: idx = 1; site = "run_init RET";     break;
    default: return;
    }

    static uint32_t counts[2];
    if (counts[idx] >= 1024u)
        return;
    counts[idx]++;

    uint64_t v0 = 0, a3 = 0, a0 = 0, a1 = 0, a2 = 0, status = 0;
    uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
    uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
    uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
    uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
    uc_reg_read(uc, UC_MIPS_REG_A2, &a2);
    uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &status);
    fprintf(stderr,
            "[INIT_EXECVE_SITE] #%u %s PC=0x%08X"
            " v0=0x%08" PRIX64 " a3=0x%08" PRIX64
            " a0=0x%08" PRIX64 " a1=0x%08" PRIX64 " a2=0x%08" PRIX64
            " STATUS=0x%08" PRIX64 " pending_excode=%u\n",
            counts[idx], site, pc,
            (uint64_t)(uint32_t)v0, (uint64_t)(uint32_t)a3,
            (uint64_t)(uint32_t)a0, (uint64_t)(uint32_t)a1,
            (uint64_t)(uint32_t)a2, status, m->pending_excode);
}

/* ------------------------------------------------------------------ */
/* vmlinux-pgui-demo (2.4.18) diagnostic probes                        */
/* ------------------------------------------------------------------ */

#define PGUI_PROBE_COUNT 26
static bool pgui_probe_fired[PGUI_PROBE_COUNT];

void pgui_probe_hook(uc_engine *uc, uint64_t address,
                            uint32_t size, void *user_data)
{
    (void)size;
    int idx = (int)(uintptr_t)user_data;
    if (idx < 0 || idx >= PGUI_PROBE_COUNT) return;
    if (pgui_probe_fired[idx]) return;
    pgui_probe_fired[idx] = true;

    static const char *names[] = {
        "start_kernel",              /* 0 */
        "prom_init",                 /* 1 */
        "prepare_namespace",         /* 2 — also reads initrd_start */
        "rd_init",                   /* 3 */
        "blk_dev_init",              /* 4 */
        "initrd_load",               /* 5 */
        "mount_root:entry",          /* 6 */
        "do_linuxrc",                /* 7 */
        "pns_no_initrd_path",        /* 8 — fires if initrd_start==0 in prepare_namespace */
        "free_initmem",              /* 9 */
        "post_mount_root",           /* 10 — reads ROOT_DEV, real_root_dev, mount_initrd */
        "change_root",               /* 11 */
        "mount_root:alloc_vfsmnt",   /* 12 — have superblock, allocating vfsmnt */
        "mount_root:graft_tree",     /* 13 — grafting new mount into namespace */
        "mount_root:set_fs_root",    /* 14 — inline set_fs_root (sw vfsmnt to current->fs) */
        "mount_root:no_super_loop",  /* 15 — get_super NULL, entering fs-type loop */
        "mount_root:read_super",     /* 16 — calling read_super in fs-type loop */
        "mount_root:blkdev_err",     /* 17 — blkdev_get failed error path */
        "mount_root:return",         /* 18 — normal return (jr ra) */
        "ext2_read_super",           /* 19 */
        "sys_execve",                /* 20 */
        "init:open_devnull",         /* 21 — syscall for open("/dev/console") */
        "init:execve_cmd",           /* 22 — syscall for execve(execute_command) */
        "init:execve_sbin_init",     /* 23 — syscall for execve("/sbin/init") */
        "init:execve_sbin_sh",       /* 24 — syscall for execve("/bin/sh") */
        "handle_sys",                /* 25 — MIPS syscall dispatch entry */
    };
    const char *name = names[idx];

    if (idx == 2) {
        /* Read initrd_start (PA 0x00258cb8) to see if it was set */
        uint32_t initrd_start_val = 0, initrd_end_val = 0;
        uc_mem_read(uc, 0x00258cb8u, &initrd_start_val, 4);
        uc_mem_read(uc, 0x00258cbcu, &initrd_end_val, 4);
        fprintf(stderr, "[PGUI24] %s  initrd_start=0x%08X  initrd_end=0x%08X\n",
                name, initrd_start_val, initrd_end_val);
    } else if (idx == 10) {
        /* After mount_root: read ROOT_DEV(0x002420a0), real_root_dev(0x00238040),
         * mount_initrd(0x0017cd4c) — all PAs = VA & 0x1fffffff */
        uint16_t root_dev = 0;
        uint32_t real_root = 0, mnt_initrd = 0;
        uc_mem_read(uc, 0x002420a0u, &root_dev,   2);
        uc_mem_read(uc, 0x00238040u, &real_root,  4);
        uc_mem_read(uc, 0x0017cd4cu, &mnt_initrd, 4);
        fprintf(stderr, "[PGUI24] %s  ROOT_DEV=0x%04X  real_root_dev=0x%08X  mount_initrd=%u\n",
                name, root_dev, real_root, mnt_initrd);
    } else if (idx == 14) {
        /* mount_root inlined set_fs_root: s2=vfsmnt, s1=current->fs */
        uint64_t s1 = 0, s2 = 0, s0 = 0;
        uc_reg_read(uc, UC_MIPS_REG_S1, &s1);
        uc_reg_read(uc, UC_MIPS_REG_S2, &s2);
        uc_reg_read(uc, UC_MIPS_REG_S0, &s0);
        fprintf(stderr, "[PGUI24] %s  s1(fs)=0x%08" PRIX64 " s2(vfsmnt)=0x%08" PRIX64
                        " s0(dentry)=0x%08" PRIX64 " PC=0x%08" PRIX64 "\n",
                name, (uint64_t)(uint32_t)s1, (uint64_t)(uint32_t)s2,
                (uint64_t)(uint32_t)s0, (uint64_t)(uint32_t)address);
    } else if (idx >= 21 && idx <= 24) {
        /* init() SYSCALL instructions: read v0 (nr) and a0 (first arg) */
        uint64_t v0 = 0, a0 = 0, a3 = 0;
        uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
        uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
        uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
        char s[96] = "<unreadable>";
        read_guest_string(uc, a0, s, sizeof(s));
        fprintf(stderr, "[PGUI24] %s  PC=0x%08" PRIX64 " nr=%u a0=0x%08" PRIX64 " \"%s\" a3=0x%08" PRIX64 "\n",
                name, (uint64_t)(uint32_t)address,
                (uint32_t)v0, (uint64_t)(uint32_t)a0, s, (uint64_t)(uint32_t)a3);
    } else if (idx == 20) {
        /* sys_execve: a0 is pt_regs pointer; try reading regs[4] (a0 = filename) */
        uint64_t a0 = 0;
        uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
        /* pt_regs->regs[4] is at offset 16 (4 regs * 4 bytes before a0 in saved regs) */
        uint32_t fname_ptr = 0;
        /* regs[4] is $a0 in saved pt_regs. On MIPS, pt_regs starts with regs[0..31]
         * so regs[4] is at offset 4*4=16 from pt_regs base. */
        uc_mem_read(uc, (uint64_t)(uint32_t)a0 + 16u, &fname_ptr, 4);
        char s[96] = "<unreadable>";
        read_guest_string(uc, fname_ptr, s, sizeof(s));
        fprintf(stderr, "[PGUI24] %s  PT_regs=0x%08" PRIX64 " fname_ptr=0x%08X \"%s\"\n",
                name, (uint64_t)(uint32_t)a0, fname_ptr, s);
    } else {
        fprintf(stderr, "[PGUI24] %s  PC=0x%08" PRIX64 "\n",
                name, (uint64_t)(uint32_t)address);
    }
}

/* ------------------------------------------------------------------ */
/* IRQ path probes                                                      */
/* ------------------------------------------------------------------ */

#define IRQ_PROBE_LIMIT 24

static int irq_probe_counts[8];

void irq_probe_hook(uc_engine *uc, uint64_t address,
                           uint32_t size, void *user_data)
{
    (void)size;
    int idx = (int)(uintptr_t)user_data;
    const char *tag = NULL;
    switch (idx) {
        case 0: tag = "vr41xx_handle_interrupt"; break;
        case 1: tag = "irq_dispatch";            break;
        case 2: tag = "do_IRQ";                  break;
        case 3: tag = "timer_interrupt";         break;
        case 4: tag = "vr41xx_timer_ack";        break;
        case 5: tag = "ll_timer_interrupt";      break;
        case 6: tag = "local_timer_interrupt";   break;
        case 7: tag = "ll_local_timer_interrupt"; break;
        default: return;
    }
    if (irq_probe_counts[idx] >= IRQ_PROBE_LIMIT)
        return;
    irq_probe_counts[idx]++;

    uint64_t a0 = 0;
    uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
    fprintf(stderr, "[IRQ_PROBE] #%d %s  PC=0x%08" PRIX64 " a0=0x%08" PRIX64 "\n",
            irq_probe_counts[idx], tag, (uint64_t)(uint32_t)address,
            (uint64_t)(uint32_t)a0);
}

/* ------------------------------------------------------------------ */
/* Exception vector probes                                            */
/* ------------------------------------------------------------------ */

#define VEC_PROBE_LIMIT 512
static int vec_probe_counts[8];

void exception_vector_probe_hook(uc_engine *uc, uint64_t address,
                                        uint32_t size, void *user_data)
{
    (void)size;
    machine_t *m = user_data;
    uint32_t va = (uint32_t)address;
    int idx = -1;
    const char *tag = NULL;
    if (va == 0x80000000u)      { idx = 0; tag = "refill"; }
    else if (va == 0x80000080u) { idx = 1; tag = "xtlb_refill"; }
    else if (va == 0x80000180u) { idx = 2; tag = "general"; }
    else if (va == 0x80000048u) { idx = 3; tag = "refill_tlbwr"; }
    else if (va == 0x8000004Cu) { idx = 4; tag = "refill_eret"; }
    else if (va == 0x00000000u) { idx = 5; tag = "bev1_refill"; }
    else if (va == 0x00000180u) { idx = 6; tag = "bev1_general"; }
    else if (va == 0x80000008u) { idx = 7; tag = "nested_refill"; }
    if (idx < 0)
        return;
    if (!tlb_trace_window_active(m))
        return;
    if (va == 0x80000180u && m->pending_excode == 1u)
        return; /* Skip normal timer IRQ traffic once late tracing is armed. */
    if (vec_probe_counts[idx] >= VEC_PROBE_LIMIT)
        return;
    vec_probe_counts[idx]++;

    uint64_t status = 0, sp = 0, k0 = 0, k1 = 0;
    uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &status);
    uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
    uc_reg_read(uc, UC_MIPS_REG_K0, &k0);
    uc_reg_read(uc, UC_MIPS_REG_K1, &k1);
    fprintf(stderr,
            "[VEC_PROBE] #%d %s PC=0x%08X STATUS=0x%08" PRIX64
            " sp=0x%08" PRIX64 " k0=0x%08" PRIX64 " k1=0x%08" PRIX64
            " pending_excode=%u hi=0x%08" PRIX64 " lo0=0x%08" PRIX64
            " lo1=0x%08" PRIX64 " badv=0x%08" PRIX64 "\n",
            vec_probe_counts[idx], tag, va, status,
            (uint64_t)(uint32_t)sp, (uint64_t)(uint32_t)k0,
            (uint64_t)(uint32_t)k1, m->pending_excode,
            (uint64_t)(uint32_t)m->shadow_cp0_entryhi,
            (uint64_t)(uint32_t)m->shadow_cp0_entrylo0,
            (uint64_t)(uint32_t)m->shadow_cp0_entrylo1,
            (uint64_t)(uint32_t)m->shadow_cp0_badvaddr);
}

/* ------------------------------------------------------------------ */
/* Page-fault probes                                                   */
/* ------------------------------------------------------------------ */

#define PF_PROBE_LIMIT 64
static int pf_probe_count = 0;

void page_fault_probe_hook(uc_engine *uc, uint64_t address,
                                  uint32_t size, void *user_data)
{
    (void)size;
    machine_t *m = user_data;
    if (pf_probe_count >= PF_PROBE_LIMIT)
        return;
    pf_probe_count++;
    uint64_t regs = 0, write_flag = 0, badv_arg = 0;
    uc_reg_read(uc, UC_MIPS_REG_A0, &regs);
    uc_reg_read(uc, UC_MIPS_REG_A1, &write_flag);
    uc_reg_read(uc, UC_MIPS_REG_A2, &badv_arg);
    const char *name = ((uint32_t)address == K24_DO_PAGE_FAULT) ? "do_page_fault_2.4" :
                       ((uint32_t)address == 0x80016ef0u) ? "do_page_fault_alt" :
                       ((uint32_t)address == K24_HANDLE_TLBL) ? "handle_tlbl_2.4" :
                       ((uint32_t)address == K24_NOPAGE_TLBL) ? "nopage_tlbl_2.4" :
                       ((uint32_t)address == K24_HANDLE_TLBS) ? "handle_tlbs_2.4" :
                       ((uint32_t)address == 0x8001a4e0u) ? "handle_tlbl_alt"   :
                       ((uint32_t)address == 0x8001a660u) ? "handle_tlbs_alt"   :
                       ((uint32_t)address == 0x800111a0u) ? "handle_sys"    :
                       ((uint32_t)address == 0x00000000u) ? "entry_zero"    : "unknown_fault";
    fprintf(stderr,
            "[PF_PROBE] #%d %s PC=0x%08" PRIX64 " regs=0x%08" PRIX64 " write=%u"
            " arg_badv=0x%08" PRIX64 " shadow_hi=0x%08" PRIX64 " shadow_badv=0x%08" PRIX64
            " pending_excode=%u\n",
            pf_probe_count, name, (uint64_t)(uint32_t)address,
            (uint64_t)(uint32_t)regs, (unsigned)((write_flag & 1u) != 0),
            (uint64_t)(uint32_t)badv_arg,
            (uint64_t)(uint32_t)m->shadow_cp0_entryhi,
            (uint64_t)(uint32_t)m->shadow_cp0_badvaddr,
            m->pending_excode);
}

/* ------------------------------------------------------------------ */
/* ICU MSYSINT1 ETIME fixup                                              */
/* ------------------------------------------------------------------ */

/*
 * vr41xx_icu_init (called from arch_init_irq → vr41xx_init_IRQ) correctly
 * sets icu1_base = 0xAF000080 in kernel data, but then writes 0x0000 to
 * MSYSINT1REG — disabling ALL SYSINT1 interrupt sources, including ETIME
 * (bit 3).
 *
 * The timer was registered via setup_irq(11, …) during time_init(), but
 * at that point icu1_base may have been 0, so the enable_irq write was
 * silently lost.  Subsequent read-modify-write sequences on MSYSINT1REG
 * do not re-enable ETIME because it was never in the initial mask.
 *
 * Fix: intercept the `jr $ra` return instruction of vr41xx_icu_init at
 * 0x80275bec (confirmed by objdump).  At that point the function body has
 * already run (msysint1 = 0), so we force bit 3 (ETIME) back on before
 * control returns to the caller.  The one-shot guard prevents repeated
 * fires (both the normal and error paths share this return site).
 */
static bool icu_etime_fixup_fired = false;

void icu_etime_fixup_hook(uc_engine *uc, uint64_t address,
                                  uint32_t size, void *user_data)
{
    (void)uc; (void)address; (void)size;
    if (icu_etime_fixup_fired) return;
    icu_etime_fixup_fired = true;
    machine_t *m = user_data;
    m->icu.msysint1 |= ICU_SRC1_ETIME;
    fprintf(stderr,
            "[ICU_FIXUP] vr41xx_icu_init returning; forced ETIME in MSYSINT1=0x%04X\n",
            m->icu.msysint1);
}

void probes_register_hooks(machine_t *m)
{
    uc_hook hk;

        memset(checkpoint_fired, 0, sizeof(checkpoint_fired));
        for (int i = 0; i < CHECKPOINT_COUNT; i++) {
            uint64_t va = mips_sext(checkpoint_table[i].va);
            uc_hook_add(m->uc, &hk, UC_HOOK_CODE, checkpoint_hook,
                        (void *)(uintptr_t)i, va, va);
        }
        {
            uint64_t va = mips_sext(0x80001598u); /* run_init_process */
            uc_hook_add(m->uc, &hk, UC_HOOK_CODE, run_init_entry_trace_hook, m, va, va);
        }
        /* run_init_process execve site probes (real failing path). */
        {
            static const uint32_t execve_sites[] = {
                RUN_INIT_SYSCALL_EPC,
                RUN_INIT_SYSCALL_RET_PC,
            };
            for (int i = 0; i < 2; i++) {
                uint64_t va = mips_sext(execve_sites[i]);
                uc_hook_add(m->uc, &hk, UC_HOOK_CODE, init_execve_site_probe_hook, m, va, va);
            }
        }

        /* vmlinux-pgui-demo (2.4.18) one-shot probes */
        memset(pgui_probe_fired, 0, sizeof(pgui_probe_fired));
        {
            static const struct { uint32_t va; int idx; } pgui_probes[] = {
                { 0x8015873cu,  0 },  /* start_kernel       */
                { 0x801651a8u,  1 },  /* prom_init          */
                { 0x800015d0u,  2 },  /* prepare_namespace  */
                { 0x80161234u,  3 },  /* rd_init            */
                { 0x80160fc4u,  4 },  /* blk_dev_init       */
                { 0x80161c5cu,  5 },  /* initrd_load        */
                { 0x8015dd30u,  6 },  /* mount_root:entry   */
                { 0x800014dcu,  7 },  /* do_linuxrc         */
                { 0x80001764u,  8 },  /* pns_no_initrd_path (branch when initrd_start==0) */
                { 0x8000a90cu,  9 },  /* free_initmem       */
                { 0x80001634u, 10 },  /* post_mount_root (reads ROOT_DEV, real_root_dev) */
                { 0x8015ecbcu, 11 },  /* change_root        */
                /* mount_root internals */
                { 0x8015deb8u, 12 },  /* mount_root:alloc_vfsmnt (got superblock) */
                { 0x8015df74u, 13 },  /* mount_root:graft_tree                    */
                { 0x8015dfe8u, 14 },  /* mount_root:set_fs_root (sw s2,20(s1))   */
                { 0x8015e328u, 15 },  /* mount_root:no_super_loop (fs-type loop)  */
                { 0x8015e3d4u, 16 },  /* mount_root:read_super in fs-type loop    */
                { 0x8015e460u, 17 },  /* mount_root:blkdev_get failed             */
                { 0x8015e280u, 18 },  /* mount_root:return (jr ra)                */
                /* filesystem */
                { 0x80071928u, 19 },  /* ext2_read_super                          */
                /* execve path */
                { 0x80007394u, 20 },  /* sys_execve                               */
                /* init() SYSCALL instructions (probe fires before syscall executes) */
                { 0x800017a4u, 21 },  /* init:open("/dev/console")                */
                { 0x8000181cu, 22 },  /* init:execve(execute_command)             */
                { 0x8000184cu, 23 },  /* init:execve("/sbin/init")                */
                { 0x800018acu, 24 },  /* init:execve("/bin/sh") — last attempt    */
                { 0x80007ac0u, 25 },  /* handle_sys (MIPS syscall dispatch)       */
            };
            for (int i = 0; i < PGUI_PROBE_COUNT; i++) {
                uint64_t va = mips_sext(pgui_probes[i].va);
                uc_hook_add(m->uc, &hk, UC_HOOK_CODE, pgui_probe_hook,
                            (void *)(uintptr_t)pgui_probes[i].idx, va, va);
            }
        }

        /* do_initcalls tracer: logs which function pointer is called at JALR site */
        initcall_trace_count = 0;
        {
            uint64_t jalr_va = mips_sext(0x80272874u);
            uc_hook_add(m->uc, &hk, UC_HOOK_CODE, initcall_trace_hook, NULL,
                        jalr_va, jalr_va);
        }

        /* RCU / wait_for_completion diagnostic probes (multi-fire, up to 8 each) */
        rcu_probe_wait_count     = 0;
        rcu_probe_dotimer_count  = 0;
        rcu_probe_rcucheck_count = 0;
        rcu_probe_rcuproc_count  = 0;
        rcu_probe_timerint_count = 0;
        {
            static const struct { uint32_t va; int idx; } rcu_probes[] = {
                { 0x801ab590u, 0 },  /* wait_for_completion   */
                { 0x800379a8u, 1 },  /* do_timer               */
                { 0x800428b8u, 2 },  /* rcu_check_callbacks    */
                { 0x80042780u, 3 },  /* rcu_process_callbacks  */
                { 0x8000eb30u, 4 },  /* timer_interrupt        */
            };
            for (int i = 0; i < 5; i++) {
                uint64_t va = mips_sext(rcu_probes[i].va);
                uc_hook_add(m->uc, &hk, UC_HOOK_CODE, rcu_probe_hook,
                            (void *)(uintptr_t)rcu_probes[i].idx, va, va);
            }
        }

        /* IRQ dispatch path probes (multi-fire, up to 24 each) */
        memset(irq_probe_counts, 0, sizeof(irq_probe_counts));
        {
            static const struct { uint32_t va; int idx; } irq_probes[] = {
                { 0x800076c0u, 0 },  /* vr41xx_handle_interrupt */
                { 0x80007508u, 1 },  /* irq_dispatch            */
                { 0x80009c30u, 2 },  /* do_IRQ                  */
                { 0x8000eb30u, 3 },  /* timer_interrupt         */
                { 0x80007a98u, 4 },  /* vr41xx_timer_ack        */
                { 0x8000ed88u, 5 },  /* ll_timer_interrupt      */
                { 0x8000ea38u, 6 },  /* local_timer_interrupt   */
                { 0x8000ee18u, 7 },  /* ll_local_timer_interrupt*/
            };
            for (int i = 0; i < 8; i++) {
                uint64_t va = mips_sext(irq_probes[i].va);
                uc_hook_add(m->uc, &hk, UC_HOOK_CODE, irq_probe_hook,
                            (void *)(uintptr_t)irq_probes[i].idx, va, va);
            }
        }

        /* Syscall dispatch and return probes. */
        {
            uint64_t va_sys = mips_sext(0x800111a0u); /* handle_sys */
            uc_hook_add(m->uc, &hk, UC_HOOK_CODE, page_fault_probe_hook, m, va_sys, va_sys);
        }
        {
            uint64_t va_zero = 0; /* User entry point 0? */
            uc_hook_add(m->uc, &hk, UC_HOOK_CODE, page_fault_probe_hook, m, va_zero, va_zero);
        }

        /* Exception vector probes (TLB refill + general exception vectors). */
        memset(vec_probe_counts, 0, sizeof(vec_probe_counts));
        {
            static const uint32_t vecs[] = {
                0x80000000u, /* refill vector */
                0x80000080u, /* xtlb refill   */
                0x80000180u, /* general       */
                0x80000048u, /* refill_tlbwr  */
                0x8000004Cu, /* refill_eret   */
                0x00000000u, /* BEV=1 refill  */
                0x00000180u, /* BEV=1 general */
                0x80000008u, /* nested refill?*/
            };
            for (int i = 0; i < 8; i++) {
                uint64_t va = mips_sext(vecs[i]);
                uc_hook_add(m->uc, &hk, UC_HOOK_CODE, exception_vector_probe_hook,
                            m, va, va);
            }
        }

        /* Page-fault probe: log the first few fault-path entries for 2.4/2.6 kernels. */
        pf_probe_count = 0;
        {
            static const uint32_t fault_sites[] = {
                K24_DO_PAGE_FAULT,
                K24_HANDLE_TLBL,
                K24_NOPAGE_TLBL,
                K24_HANDLE_TLBS,
                0x80016ef0u, /* alt do_page_fault */
                0x8001a4e0u, /* alt handle_tlbl   */
                0x8001a660u, /* alt handle_tlbs   */
            };
            for (size_t i = 0; i < (sizeof(fault_sites) / sizeof(fault_sites[0])); i++) {
                uint64_t va = mips_sext(fault_sites[i]);
                uc_hook_add(m->uc, &hk, UC_HOOK_CODE, page_fault_probe_hook, m, va, va);
            }
        }

        /* ICU ETIME fixup: force-enable ETIME bit in MSYSINT1 after
         * vr41xx_icu_init clears it.  Fires at the jr $ra (0x80275bec). */
        icu_etime_fixup_fired = false;
        {
            uint64_t va = mips_sext(0x80275becu);
            uc_hook_add(m->uc, &hk, UC_HOOK_CODE, icu_etime_fixup_hook, m, va, va);
        }
}
