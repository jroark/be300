#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "be300_probe.h"

#include "cpu.h"
#include "machine.h"

/*
 * Small, supported diagnostics for BE-300 emulator runs.
 *
 * This file intentionally contains only reusable default-off diagnostics:
 *   - --mmio-coverage: first-hit and shutdown summary for modeled MMIO ranges
 *   - --detect-stall: low-overhead tight-PC-set sampler
 *
 * Historical lifecycle/GDI investigation probes were removed because they were
 * pass-specific and produced bitmap artifacts in the repository root.
 */

typedef struct {
    const char *dev;
    uint32_t    off;
    uint32_t    first_pc;
    uint32_t    hits;
    uint16_t    len;
    uint8_t     op;
    uint8_t     mclass;
} mmio_cov_entry_t;

enum {
    BE300_MMIO_COV_CAP = 4096u,
    BE300_STALL_PC_CAP = 4096u,
    BE300_STALL_HIGH_HYSTERESIS = 8u,
};

static struct {
    struct machine *machine;
    bool mmio_coverage;
    bool detect_stall;
    uint32_t stall_window;
    uint32_t stall_unique_threshold;
    uint32_t stall_wall_secs;
} g_probe;

static mmio_cov_entry_t g_mmio_cov[BE300_MMIO_COV_CAP];
static uint32_t g_mmio_cov_used;
static uint32_t g_mmio_cov_overflow;

static struct {
    uint32_t pcs[BE300_STALL_PC_CAP];
    uint32_t inserted;
    uint64_t insn_in_bucket;
    uint64_t low_run_start_ns;
    uint32_t consecutive_high_buckets;
    uint64_t last_fire_ns;
    uint32_t fire_count;
    uint32_t last_fire_pc;
} g_stall;

static const char *be300_mmio_class_name(int mclass)
{
    switch (mclass) {
    case BE300_MMIO_CLASS_KNOWN:   return "known";
    case BE300_MMIO_CLASS_STUBBED: return "stubbed";
    case BE300_MMIO_CLASS_LATCHED: return "latched";
    case BE300_MMIO_CLASS_DEFAULT: return "default";
    default:                       return "?";
    }
}

static uint32_t be300_mmio_hash(const char *dev, uint32_t off, char op)
{
    uint32_t h = 5381u;

    if (dev) {
        const unsigned char *p = (const unsigned char *)dev;
        while (*p)
            h = ((h << 5) + h) ^ *p++;
    }

    h ^= off * 2654435761u;
    h ^= (uint32_t)(unsigned char)op;
    return h & (BE300_MMIO_COV_CAP - 1u);
}

static uint64_t be300_probe_wall_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;

    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
        (uint64_t)ts.tv_nsec;
}

static uint32_t be300_probe_norm_pc(uint64_t pc)
{
    return (uint32_t)pc;
}

static bool be300_probe_cpu_is_ours(const struct cpu *cpu)
{
    return cpu && g_probe.machine && cpu->machine == g_probe.machine;
}

static void be300_probe_reset_state(void)
{
    memset(g_mmio_cov, 0, sizeof(g_mmio_cov));
    g_mmio_cov_used = 0;
    g_mmio_cov_overflow = 0;
    memset(&g_stall, 0, sizeof(g_stall));
}

void be300_probe_attach(struct machine *machine)
{
    g_probe.machine = machine;
    g_probe.mmio_coverage = false;
    g_probe.detect_stall = false;
    g_probe.stall_window = 0;
    g_probe.stall_unique_threshold = 0;
    g_probe.stall_wall_secs = 0;
    be300_probe_reset_state();
}

void be300_probe_set_options(bool mmio_coverage,
                             bool detect_stall,
                             uint32_t stall_window,
                             uint32_t stall_unique_threshold,
                             uint32_t stall_wall_secs)
{
    g_probe.mmio_coverage = mmio_coverage;
    g_probe.detect_stall = detect_stall;
    g_probe.stall_window = stall_window ? stall_window : 10000u;
    g_probe.stall_unique_threshold =
        stall_unique_threshold ? stall_unique_threshold : 64u;
    g_probe.stall_wall_secs = stall_wall_secs ? stall_wall_secs : 5u;

    if (g_probe.mmio_coverage)
        fprintf(stderr, "[BE300_MMIO_COVERAGE] enabled cap=%u\n",
            BE300_MMIO_COV_CAP);

    if (g_probe.detect_stall)
        fprintf(stderr,
            "[BE300_STALL] sampler enabled window=%u threshold=%u wall_secs=%u\n",
            g_probe.stall_window, g_probe.stall_unique_threshold,
            g_probe.stall_wall_secs);
}

void be300_probe_note_mmio(const char *dev, uint32_t off, char op,
    uint32_t len, uint64_t pc, int mclass)
{
    uint32_t slot;

    if (!g_probe.mmio_coverage || !dev)
        return;

    slot = be300_mmio_hash(dev, off, op);
    for (uint32_t probes = 0; probes < BE300_MMIO_COV_CAP; probes++) {
        mmio_cov_entry_t *e = &g_mmio_cov[slot];

        if (e->dev == NULL) {
            if (g_mmio_cov_used + 1u >= BE300_MMIO_COV_CAP) {
                g_mmio_cov_overflow++;
                return;
            }

            e->dev = dev;
            e->off = off;
            e->first_pc = be300_probe_norm_pc(pc);
            e->hits = 1;
            e->len = (uint16_t)len;
            e->op = (uint8_t)op;
            e->mclass = (uint8_t)mclass;
            g_mmio_cov_used++;
            fprintf(stderr,
                "[BE300_MMIO_FIRST] dev=%s off=0x%05x op=%c len=%u class=%s pc=0x%08x\n",
                dev, off, op, len, be300_mmio_class_name(mclass),
                e->first_pc);
            return;
        }

        if (e->op == (uint8_t)op && e->off == off &&
            (e->dev == dev || strcmp(e->dev, dev) == 0)) {
            e->hits++;
            return;
        }

        slot = (slot + 1u) & (BE300_MMIO_COV_CAP - 1u);
    }

    g_mmio_cov_overflow++;
}

static void be300_probe_stall_sample(uint32_t pc)
{
    uint32_t slot;

    if (pc == 0)
        pc = 1;

    slot = (pc * 2654435761u) & (BE300_STALL_PC_CAP - 1u);
    for (uint32_t probes = 0; probes < BE300_STALL_PC_CAP; probes++) {
        if (g_stall.pcs[slot] == 0) {
            g_stall.pcs[slot] = pc;
            g_stall.inserted++;
            break;
        }
        if (g_stall.pcs[slot] == pc)
            break;
        slot = (slot + 1u) & (BE300_STALL_PC_CAP - 1u);
    }

    g_stall.insn_in_bucket++;
    if (g_stall.insn_in_bucket < g_probe.stall_window)
        return;

    uint64_t now_ns = be300_probe_wall_ns();
    if (g_stall.inserted < g_probe.stall_unique_threshold) {
        if (g_stall.low_run_start_ns == 0)
            g_stall.low_run_start_ns = now_ns;
        g_stall.consecutive_high_buckets = 0;

        if (now_ns - g_stall.low_run_start_ns >=
            (uint64_t)g_probe.stall_wall_secs * UINT64_C(1000000000) &&
            now_ns - g_stall.last_fire_ns >= UINT64_C(30000000000)) {
            fprintf(stderr,
                "[BE300_STALL] fire=%u unique=%u window=%u pc=0x%08x low_run_secs=%llu\n",
                g_stall.fire_count + 1u, g_stall.inserted,
                g_probe.stall_window, pc,
                (unsigned long long)((now_ns - g_stall.low_run_start_ns) /
                    UINT64_C(1000000000)));
            g_stall.fire_count++;
            g_stall.last_fire_ns = now_ns;
            g_stall.last_fire_pc = pc;
        }
    } else {
        g_stall.consecutive_high_buckets++;
        if (g_stall.consecutive_high_buckets >= BE300_STALL_HIGH_HYSTERESIS)
            g_stall.low_run_start_ns = 0;
    }

    memset(g_stall.pcs, 0, sizeof(g_stall.pcs));
    g_stall.inserted = 0;
    g_stall.insn_in_bucket = 0;
}

void be300_probe_note_exec(struct cpu *cpu, uint64_t pc)
{
    if (g_probe.detect_stall && be300_probe_cpu_is_ours(cpu))
        be300_probe_stall_sample(be300_probe_norm_pc(pc));
}

static int be300_mmio_cov_cmp_desc(const void *a, const void *b)
{
    const mmio_cov_entry_t *ea = (const mmio_cov_entry_t *)a;
    const mmio_cov_entry_t *eb = (const mmio_cov_entry_t *)b;

    if (ea->hits == 0 && eb->hits == 0) return 0;
    if (ea->hits == 0) return 1;
    if (eb->hits == 0) return -1;
    if (eb->hits > ea->hits) return 1;
    if (eb->hits < ea->hits) return -1;
    if (ea->mclass != eb->mclass)
        return (int)eb->mclass - (int)ea->mclass;
    return strcmp(ea->dev ? ea->dev : "", eb->dev ? eb->dev : "");
}

static void be300_probe_dump_mmio_coverage(void)
{
    mmio_cov_entry_t *sorted;

    if (!g_probe.mmio_coverage)
        return;

    if (g_mmio_cov_used == 0) {
        fprintf(stderr, "[BE300_MMIO_COVERAGE] no hits recorded\n");
        return;
    }

    sorted = malloc(sizeof(g_mmio_cov));
    if (!sorted) {
        fprintf(stderr, "[BE300_MMIO_COVERAGE] malloc failed, skipping dump\n");
        return;
    }

    memcpy(sorted, g_mmio_cov, sizeof(g_mmio_cov));
    qsort(sorted, BE300_MMIO_COV_CAP, sizeof(mmio_cov_entry_t),
        be300_mmio_cov_cmp_desc);

    fprintf(stderr, "[BE300_MMIO_COVERAGE] unique=%u overflow=%u cap=%u\n",
        g_mmio_cov_used, g_mmio_cov_overflow, BE300_MMIO_COV_CAP);
    for (uint32_t i = 0; i < BE300_MMIO_COV_CAP; i++) {
        const mmio_cov_entry_t *e = &sorted[i];
        if (e->hits == 0)
            break;
        fprintf(stderr,
            "[BE300_MMIO_COVERAGE] dev=%-20s off=0x%05x op=%c len=%u class=%-7s hits=%u first_pc=0x%08x\n",
            e->dev, e->off, e->op, e->len,
            be300_mmio_class_name(e->mclass), e->hits, e->first_pc);
    }

    free(sorted);
}

void be300_probe_detach(struct machine *machine)
{
    if (machine == g_probe.machine) {
        be300_probe_dump_mmio_coverage();
        if (g_probe.detect_stall)
            fprintf(stderr,
                "[BE300_STALL_SUMMARY] fired=%u last_pc=0x%08x\n",
                g_stall.fire_count, g_stall.last_fire_pc);
    }

    g_probe.machine = NULL;
    g_probe.mmio_coverage = false;
    g_probe.detect_stall = false;
    g_probe.stall_window = 0;
    g_probe.stall_unique_threshold = 0;
    g_probe.stall_wall_secs = 0;
    be300_probe_reset_state();
}
