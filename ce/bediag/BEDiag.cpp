#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
//#include <wchar.h>

extern "C" BOOL VirtualCopy(LPVOID, LPVOID, DWORD, DWORD);

#ifndef PAGE_PHYSICAL
#define PAGE_PHYSICAL 0x40000000
#endif

#ifndef PAGE_NOCACHE
#define PAGE_NOCACHE 0x0200
#endif

#define BEDIAG_BUILD_TAG         "hwseed12"
#define BEDIAG_MAX_REGION_SIZE   0x1000
#define BEDIAG_MAX_PATH_LEN      260
#define BEDIAG_BACKLOG_SIZE      0x80000
#define BEDIAG_RAW_CHUNK_SIZE    32u
#define BEDIAG_TLB_ENTRY_COUNT   32u
enum snapshot_phase_t {
    PHASE_INIT = 0,
    PHASE_PLUS1S = 1,
    PHASE_PLUS5S = 2,
    PHASE_COUNT = 3
};

typedef struct {
    const char *name;
    DWORD base;
    DWORD size;
    BOOL emit_raw;
    BOOL ok[PHASE_COUNT];
    DWORD crc[PHASE_COUNT];
    BYTE data[PHASE_COUNT][BEDIAG_MAX_REGION_SIZE];
} bediag_region_t;

typedef struct {
    const char *name;
    DWORD base;
    DWORD size;
    BOOL emit_raw;
    BOOL ok[PHASE_COUNT];
    DWORD valid_prefix[PHASE_COUNT];
    DWORD crc[PHASE_COUNT];
    BYTE data[PHASE_COUNT][BEDIAG_MAX_REGION_SIZE];
} bediag_vregion_t;

typedef struct {
    DWORD pa;
    const char *label;
} focus_word_t;

typedef struct {
    DWORD va;
    const char *label;
} tlb_query_t;

typedef struct {
    DWORD init_tick;
    DWORD init_context;
    HANDLE worker_thread;
    DWORD worker_thread_id;
    LONG stop_requested;
    WCHAR active_key[256];
    BOOL worker_started;
} bediag_driver_t;

static const char *g_phase_names[PHASE_COUNT] = {
    "init",
    "plus1s",
    "plus5s"
};

static bediag_region_t g_regions[] = {
    { "low_sdram_0000", 0x00000000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "low_sdram_1000", 0x00001000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "low_sdram_2000", 0x00002000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "low_sdram_3000", 0x00003000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "low_sdram_4000", 0x00004000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "low_sdram_5000", 0x00005000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "low_sdram_6000", 0x00006000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "low_sdram_7000", 0x00007000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "low_sdram_8000", 0x00008000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "low_sdram_9000", 0x00009000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "low_sdram_A000", 0x0000A000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "low_sdram_B000", 0x0000B000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "low_sdram_C000", 0x0000C000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "low_sdram_D000", 0x0000D000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "low_sdram_E000", 0x0000E000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "low_sdram_F000", 0x0000F000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "ctx_high_page", 0x00001000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "callback_page", 0x00011000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "high_sdram_fd4000", 0x00FD4000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "ctx_tlb",       0x00002000u, 0x0200u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "resume_ctx",    0x00002200u, 0x0100u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "caller_frame",  0x00001700u, 0x00C0u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "ctx_stack",     0x00003780u, 0x00A0u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "bootctx",       0x00006000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "bootparam0",    0x0001D000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "bootparam1",    0x0002D000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "cb_tbl",        0x00051680u, 0x0480u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "objptr",        0x00660000u, 0x0040u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "obj_header",    0x0066BFC0u, 0x0020u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "obj_exec_page", 0x00669000u, 0x1000u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "postboot_text", 0x00679400u, 0x0200u, TRUE,  { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "vrc4173_cwin",  0x0A000C00u, 0x0050u, FALSE, { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "vr4131_safe",   0x0F000000u, 0x0120u, FALSE, { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} }
};

static bediag_vregion_t g_vregions[] = {
    { "callback_target_page", 0x00016000u, 0x1000u, TRUE, { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "callback_slot_page", 0x00017000u, 0x1000u, TRUE, { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "user_obj_page", 0x0818F000u, 0x1000u, TRUE, { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} }
};

static const focus_word_t g_focus_words[] = {
    { 0x00000000u, "low_vec_0000_00" },
    { 0x00000004u, "low_vec_0000_04" },
    { 0x00000008u, "low_vec_0000_08" },
    { 0x0000000Cu, "low_vec_0000_0c" },
    { 0x00000010u, "low_vec_0000_10" },
    { 0x00000014u, "low_vec_0000_14" },
    { 0x00000018u, "low_vec_0000_18" },
    { 0x0000001Cu, "low_vec_0000_1c" },
    { 0x00000080u, "low_vec_0080_00" },
    { 0x00000084u, "low_vec_0080_04" },
    { 0x00000088u, "low_vec_0080_08" },
    { 0x0000008Cu, "low_vec_0080_0c" },
    { 0x00000090u, "low_vec_0080_10" },
    { 0x00000094u, "low_vec_0080_14" },
    { 0x00000098u, "low_vec_0080_18" },
    { 0x0000009Cu, "low_vec_0080_1c" },
    { 0x00000100u, "low_vec_0100_00" },
    { 0x00000104u, "low_vec_0100_04" },
    { 0x00000108u, "low_vec_0100_08" },
    { 0x0000010Cu, "low_vec_0100_0c" },
    { 0x00000110u, "low_vec_0100_10" },
    { 0x00000114u, "low_vec_0100_14" },
    { 0x00000118u, "low_vec_0100_18" },
    { 0x0000011Cu, "low_vec_0100_1c" },
    { 0x00000180u, "low_vec_0180_00" },
    { 0x00000184u, "low_vec_0180_04" },
    { 0x00000188u, "low_vec_0180_08" },
    { 0x0000018Cu, "low_vec_0180_0c" },
    { 0x00000190u, "low_vec_0180_10" },
    { 0x00000194u, "low_vec_0180_14" },
    { 0x00000198u, "low_vec_0180_18" },
    { 0x0000019Cu, "low_vec_0180_1c" },
    { 0x1FC00000u, "bev_vec_0000_00" },
    { 0x1FC00004u, "bev_vec_0000_04" },
    { 0x1FC00008u, "bev_vec_0000_08" },
    { 0x1FC0000Cu, "bev_vec_0000_0c" },
    { 0x1FC00010u, "bev_vec_0000_10" },
    { 0x1FC00014u, "bev_vec_0000_14" },
    { 0x1FC00018u, "bev_vec_0000_18" },
    { 0x1FC0001Cu, "bev_vec_0000_1c" },
    { 0x1FC00200u, "bev_vec_0200_00" },
    { 0x1FC00204u, "bev_vec_0200_04" },
    { 0x1FC00208u, "bev_vec_0200_08" },
    { 0x1FC0020Cu, "bev_vec_0200_0c" },
    { 0x1FC00210u, "bev_vec_0200_10" },
    { 0x1FC00214u, "bev_vec_0200_14" },
    { 0x1FC00218u, "bev_vec_0200_18" },
    { 0x1FC0021Cu, "bev_vec_0200_1c" },
    { 0x1FC00380u, "bev_vec_0380_00" },
    { 0x1FC00384u, "bev_vec_0380_04" },
    { 0x1FC00388u, "bev_vec_0380_08" },
    { 0x1FC0038Cu, "bev_vec_0380_0c" },
    { 0x1FC00390u, "bev_vec_0380_10" },
    { 0x1FC00394u, "bev_vec_0380_14" },
    { 0x1FC00398u, "bev_vec_0380_18" },
    { 0x1FC0039Cu, "bev_vec_0380_1c" },
    { 0x0A000C30u, "nand_c30" },
    { 0x0A000C34u, "nand_c34" },
    { 0x0A000C38u, "nand_c38" },
    { 0x0A000C48u, "nand_c48" },
    { 0x0A000C4Cu, "nand_c4c" },
    { 0x0F000080u, "icu_0080" },
    { 0x0F000100u, "pmu_0100" },
    { 0x00002220u, "ctx_2220" },
    { 0x00002228u, "ctx_2228" },
    { 0x00002274u, "ctx_2274" },
    { 0x00001760u, "caller_sp20_1760" },
    { 0x00001764u, "caller_sp24_1764" },
    { 0x00001788u, "caller_s0_1788" },
    { 0x0000178Cu, "caller_ra_178c" },
    { 0x00001AB8u, "ctxhi_1ab8" },
    { 0x00001ABCu, "ctxhi_1abc" },
    { 0x00001AC0u, "ctxhi_1ac0" },
    { 0x00001AC4u, "ctxhi_1ac4" },
    { 0x00001AC8u, "ctxhi_1ac8" },
    { 0x000116B0u, "callback_old_16b0" },
    { 0x000116B4u, "callback_old_16b4" },
    { 0x000116B8u, "callback_old_16b8" },
    { 0x000116BCu, "callback_old_16bc" },
    { 0x000116C0u, "callback_old_16c0" },
    { 0x000116C4u, "callback_old_16c4" },
    { 0x00FD40E0u, "callback_slot_pa_e0" },
    { 0x00FD40E4u, "callback_slot_pa_e4" },
    { 0x00FD40E8u, "callback_slot_pa_e8" },
    { 0x00FD40ECu, "callback_slot_pa_ec" },
    { 0x00660000u, "objptr_0000" },
    { 0x0066BFC0u, "objhdr_bfc0" },
    { 0x0066BFC4u, "objhdr_bfc4" },
    { 0x0066BFC8u, "objhdr_bfc8" },
    { 0x0066BFCCu, "objhdr_bfcc" },
    { 0x006697C0u, "objexec_97c0" },
    { 0x00669A78u, "objexec_9a78" },
    { 0x00669A7Cu, "objexec_9a7c" },
    { 0x00669A80u, "objexec_9a80" },
    { 0x00669A84u, "objexec_9a84" },
    { 0x00669A88u, "objexec_9a88" },
    { 0x006794ECu, "resume_glob_94ec" },
    { 0x006794F0u, "resume_glob_94f0" },
    { 0x006794F4u, "resume_glob_94f4" },
    { 0x006794F8u, "resume_glob_94f8" }
};

static const focus_word_t g_virtual_focus_words[] = {
    { 0x000162A0u, "callback_62a0" },
    { 0x000162A4u, "callback_62a4" },
    { 0x000162A8u, "callback_62a8" },
    { 0x000162ACu, "callback_62ac" },
    { 0x000162B0u, "callback_62b0" },
    { 0x000162B4u, "callback_62b4" },
    { 0x000170E0u, "callback_slot_70e0" },
    { 0x000170E4u, "callback_slot_70e4" },
    { 0x000170E8u, "callback_slot_70e8" },
    { 0x000170ECu, "callback_slot_70ec" },
    { 0x0818FB68u, "userobj_fb68" },
    { 0x0818FC20u, "userobj_fc20" },
    { 0x0818FE20u, "userobj_fe20" },
    { 0x0818FE24u, "userobj_fe24" },
    { 0x0818FE28u, "userobj_fe28" },
    { 0x0818FE2Cu, "userobj_fe2c" },
    { 0x0818FE30u, "userobj_fe30" }
};

static const tlb_query_t g_tlb_queries[] = {
    { 0x00016000u, "callback_target_page" },
    { 0x00017000u, "callback_slot_page" },
    { 0x0818F000u, "user_obj_page" },
    { 0xFFFFD000u, "helper_high_page" }
};

static bediag_driver_t g_driver;
static HANDLE g_serial = INVALID_HANDLE_VALUE;
static HANDLE g_file = INVALID_HANDLE_VALUE;
static DWORD g_serial_open_error = 0;
static BOOL g_serial_retry_done = FALSE;
static BOOL g_serial_failure_reported = FALSE;
static BOOL g_had_error = FALSE;
static BOOL g_backlog_overflow = FALSE;
static LONG g_driver_active = 0;
static LONG g_driver_refcount = 0;
static char g_backlog[BEDIAG_BACKLOG_SIZE];
static DWORD g_backlog_used = 0;
static WCHAR g_file_path[BEDIAG_MAX_PATH_LEN];
static const WCHAR g_breadcrumb_key[] = L"Drivers\\BuiltIn\\BEDiag";
static const WCHAR *g_primary_file_paths[] = {
    L"\\Nand Disk\\BEDiag_boot.txt",
    L"\\Storage Card\\BEDiag_boot.txt"
};

static const WCHAR *g_file_roots[] = {
    L"\\Nand Disk",
    L"\\Storage Card"
};

static void Logf(const char *fmt, ...);

static void ReadTlbEntryAsm(DWORD index, DWORD *out_words)
{
    (void)index;
    (void)out_words;
    __asm(
        ".set noreorder;"
        "mfc0 t4, $0;"
        "mfc0 t5, $2;"
        "mfc0 t6, $3;"
        "mfc0 t7, $5;"
        "mfc0 t8, $10;"
        "mtc0 a0, $0;"
        "nop;"
        "nop;"
        "tlbr;"
        "nop;"
        "nop;"
        "mfc0 t0, $2;"
        "mfc0 t1, $3;"
        "mfc0 t2, $5;"
        "mfc0 t3, $10;"
        "sw t0, 0(a1);"
        "sw t1, 4(a1);"
        "sw t2, 8(a1);"
        "sw t3, 12(a1);"
        "mtc0 t4, $0;"
        "mtc0 t5, $2;"
        "mtc0 t6, $3;"
        "mtc0 t7, $5;"
        "mtc0 t8, $10;"
        "nop;"
        "nop;"
        ".set reorder;"
    );
}

static void CopyWide(WCHAR *dst, DWORD dst_cch, const WCHAR *src)
{
    DWORD i;
    if (!dst || dst_cch == 0)
        return;
    dst[0] = L'\0';
    if (!src)
        return;
    for (i = 0; i + 1 < dst_cch && src[i] != L'\0'; i++)
        dst[i] = src[i];
    dst[i] = L'\0';
}

static unsigned __int64 TlbPairBytesFromPageMask(DWORD pagemask)
{
    unsigned __int64 pair_bytes;

    pair_bytes = ((unsigned __int64)(pagemask | 0x1FFFu) + 1u);
    if (pair_bytes < 0x2000u)
        pair_bytes = 0x2000u;
    if ((pair_bytes & 0xFFFu) != 0u)
        pair_bytes = (pair_bytes + 0xFFFu) & ~(unsigned __int64)0xFFFu;
    if ((pair_bytes & (pair_bytes - 1u)) != 0u) {
        unsigned __int64 p2;
        p2 = 0x2000u;
        while (p2 < pair_bytes && p2 < (((unsigned __int64)1) << 31))
            p2 <<= 1;
        pair_bytes = p2;
    }
    return pair_bytes;
}

static unsigned __int64 TlbLeafBytesFromPageMask(DWORD pagemask)
{
    unsigned __int64 pair_bytes;

    pair_bytes = TlbPairBytesFromPageMask(pagemask);
    if (pair_bytes < 0x2000u)
        return 0x1000u;
    return pair_bytes >> 1;
}

static BOOL TlbEntryMatchesVa(DWORD va, DWORD entryhi, DWORD pagemask)
{
    unsigned __int64 pair_bytes;
    DWORD mask;

    pair_bytes = TlbPairBytesFromPageMask(pagemask);
    mask = ~((DWORD)(pair_bytes - 1u));
    return ((va & mask) == (entryhi & mask));
}

static void DumpTlbPhase(snapshot_phase_t phase)
{
    DWORD entries[BEDIAG_TLB_ENTRY_COUNT][4];
    DWORD i;
    BOOL any_nonzero;

    memset(entries, 0, sizeof(entries));
    any_nonzero = FALSE;

    for (i = 0; i < BEDIAG_TLB_ENTRY_COUNT; i++) {
        ReadTlbEntryAsm(i, entries[i]);
        if ((entries[i][0] | entries[i][1] | entries[i][2] | entries[i][3]) != 0u)
            any_nonzero = TRUE;
    }

    Logf("[TLB_TABLE] phase=%s slots=%lu any_nonzero=%u\r\n",
         g_phase_names[phase],
         BEDIAG_TLB_ENTRY_COUNT,
         any_nonzero ? 1u : 0u);

    for (i = 0; i < BEDIAG_TLB_ENTRY_COUNT; i++) {
        DWORD lo0, lo1, mask, hi;
        lo0 = entries[i][0];
        lo1 = entries[i][1];
        mask = entries[i][2];
        hi = entries[i][3];
        if ((lo0 | lo1 | mask | hi) == 0u)
            continue;
        Logf("[TLB_ENTRY] phase=%s idx=%lu lo0=0x%08lX lo1=0x%08lX mask=0x%08lX hi=0x%08lX\r\n",
             g_phase_names[phase], i, lo0, lo1, mask, hi);
    }

    for (i = 0; i < (sizeof(g_tlb_queries) / sizeof(g_tlb_queries[0])); i++) {
        DWORD idx;
        BOOL matched;
        matched = FALSE;
        for (idx = 0; idx < BEDIAG_TLB_ENTRY_COUNT; idx++) {
            DWORD lo0, lo1, mask, hi, lo;
            unsigned __int64 leaf_bytes, pa_page, pfn;
            BOOL odd_page, valid;

            lo0 = entries[idx][0];
            lo1 = entries[idx][1];
            mask = entries[idx][2];
            hi = entries[idx][3];
            if ((lo0 | lo1 | mask | hi) == 0u)
                continue;
            if (!TlbEntryMatchesVa(g_tlb_queries[i].va, hi, mask))
                continue;

            matched = TRUE;
            leaf_bytes = TlbLeafBytesFromPageMask(mask);
            odd_page = ((leaf_bytes != 0u) &&
                        ((((unsigned __int64)g_tlb_queries[i].va) & leaf_bytes) != 0u)) ? TRUE : FALSE;
            lo = odd_page ? lo1 : lo0;
            valid = (lo & 0x2u) != 0u;
            pfn = ((unsigned __int64)((lo >> 6) & 0xFFFFFu)) << 10;
            if (leaf_bytes != 0u)
                pa_page = pfn & ~(leaf_bytes - 1u);
            else
                pa_page = pfn;

            Logf("[TLB_MATCH] phase=%s label=%s VA=0x%08lX idx=%lu hi=0x%08lX mask=0x%08lX lo=0x%08lX odd=%u valid=%u global=%u asid=0x%02lX leaf_bytes=0x%08I64X pa_page=0x%08I64X\r\n",
                 g_phase_names[phase],
                 g_tlb_queries[i].label,
                 g_tlb_queries[i].va,
                 idx,
                 hi,
                 mask,
                 lo,
                 odd_page ? 1u : 0u,
                 valid ? 1u : 0u,
                 ((lo0 & 0x1u) != 0u && (lo1 & 0x1u) != 0u) ? 1u : 0u,
                 hi & 0xFFu,
                 leaf_bytes,
                 pa_page);
        }
        if (!matched) {
            Logf("[TLB_MATCH] phase=%s label=%s VA=0x%08lX status=NO_MATCH\r\n",
                 g_phase_names[phase],
                 g_tlb_queries[i].label,
                 g_tlb_queries[i].va);
        }
    }
}

static void CopyAnsi(char *dst, DWORD dst_cch, const char *src)
{
    DWORD i;
    if (!dst || dst_cch == 0)
        return;
    dst[0] = '\0';
    if (!src)
        return;
    for (i = 0; i + 1 < dst_cch && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static void WideToAnsi(const WCHAR *src, char *dst, int dst_size)
{
    int ok;
    if (!dst || dst_size <= 0)
        return;
    dst[0] = '\0';
    if (!src)
        return;
    ok = WideCharToMultiByte(CP_ACP, 0, src, -1, dst, dst_size, NULL, NULL);
    if (ok <= 0)
        _snprintf(dst, dst_size - 1, "<conv-failed>");
    dst[dst_size - 1] = '\0';
}

static void WideBytesToAnsi(const WCHAR *src, DWORD src_wchars_max, char *dst, int dst_size)
{
    DWORD wlen;
    int out;

    if (!dst || dst_size <= 0)
        return;
    dst[0] = '\0';
    if (!src || src_wchars_max == 0)
        return;

    wlen = 0;
    while (wlen < src_wchars_max && src[wlen] != L'\0')
        wlen++;

    out = WideCharToMultiByte(CP_ACP, 0, src, (int)wlen, dst, dst_size - 1, NULL, NULL);
    if (out <= 0) {
        _snprintf(dst, dst_size - 1, "<conv-failed>");
        dst[dst_size - 1] = '\0';
        return;
    }
    dst[out] = '\0';
}

static BOOL DirectoryExists(const WCHAR *path)
{
    DWORD attrs;
    if (!path || path[0] == L'\0')
        return FALSE;
    attrs = GetFileAttributes(path);
    if (attrs == 0xFFFFFFFFu)
        return FALSE;
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? TRUE : FALSE;
}

static DWORD ReadLE32(const BYTE *p)
{
    return ((DWORD)p[0]) |
           ((DWORD)p[1] << 8) |
           ((DWORD)p[2] << 16) |
           ((DWORD)p[3] << 24);
}

static DWORD Crc32(const BYTE *data, DWORD size)
{
    DWORD crc;
    DWORD i;
    crc = 0xFFFFFFFFu;
    for (i = 0; i < size; i++) {
        DWORD j;
        crc ^= (DWORD)data[i];
        for (j = 0; j < 8; j++) {
            if (crc & 1u)
                crc = (crc >> 1) ^ 0xEDB88320u;
            else
                crc >>= 1;
        }
    }
    return ~crc;
}

static void AppendBacklog(const char *text, DWORD len)
{
    DWORD space;
    if (!text || len == 0)
        return;
    if (g_backlog_used >= BEDIAG_BACKLOG_SIZE) {
        g_backlog_overflow = TRUE;
        return;
    }
    space = BEDIAG_BACKLOG_SIZE - g_backlog_used;
    if (len > space) {
        len = space;
        g_backlog_overflow = TRUE;
    }
    memcpy(g_backlog + g_backlog_used, text, len);
    g_backlog_used += len;
}

static void WriteToSink(HANDLE h, const char *text, DWORD len)
{
    DWORD written;
    if (h == INVALID_HANDLE_VALUE || !text || len == 0)
        return;
    written = 0;
    WriteFile(h, text, len, &written, NULL);
}

static void FlushSinks(void)
{
    if (g_serial != INVALID_HANDLE_VALUE)
        FlushFileBuffers(g_serial);
    if (g_file != INVALID_HANDLE_VALUE)
        FlushFileBuffers(g_file);
}

static void EmitLog(const char *text)
{
    DWORD len;
    if (!text)
        return;
    len = (DWORD)strlen(text);
    AppendBacklog(text, len);
    WriteToSink(g_serial, text, len);
    WriteToSink(g_file, text, len);
}

static void PrimeFileSink(void)
{
    DWORD written;

    if (g_file == INVALID_HANDLE_VALUE)
        return;

    written = 0;
    if (g_backlog_used != 0)
        WriteFile(g_file, g_backlog, g_backlog_used, &written, NULL);
    if (g_backlog_overflow) {
        const char *overflow = "[BEDIAG] backlog_overflow=1\r\n";
        WriteFile(g_file, overflow, (DWORD)strlen(overflow), &written, NULL);
    }
    FlushFileBuffers(g_file);
}

static void Logf(const char *fmt, ...)
{
    char buffer[2048];
    int n;
    va_list args;

    va_start(args, fmt);
    n = _vsnprintf(buffer, sizeof(buffer) - 1, fmt, args);
    va_end(args);

    if (n < 0)
        buffer[sizeof(buffer) - 1] = '\0';
    else
        buffer[n] = '\0';

    EmitLog(buffer);
}

static BOOL OpenSerialLog(void)
{
    DCB dcb;

    if (g_serial != INVALID_HANDLE_VALUE)
        return TRUE;

    g_serial = CreateFile(L"COM1:", GENERIC_READ | GENERIC_WRITE,
                          0, NULL, OPEN_EXISTING, 0, NULL);
    if (g_serial == INVALID_HANDLE_VALUE) {
        g_serial_open_error = GetLastError();
        return FALSE;
    }

    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (GetCommState(g_serial, &dcb)) {
        dcb.BaudRate = CBR_115200;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        SetCommState(g_serial, &dcb);
    }

    return TRUE;
}

static void SetBreadcrumbDWORD(const WCHAR *name, DWORD value)
{
    HKEY hKey;

    hKey = NULL;
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, g_breadcrumb_key, 0, NULL, 0, 0, NULL, &hKey, NULL) != ERROR_SUCCESS)
        return;

    RegSetValueEx(hKey, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
    RegCloseKey(hKey);
}

static void SetBreadcrumbString(const WCHAR *name, const WCHAR *value)
{
    HKEY hKey;
    DWORD cbData;

    if (!value)
        return;

    hKey = NULL;
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, g_breadcrumb_key, 0, NULL, 0, 0, NULL, &hKey, NULL) != ERROR_SUCCESS)
        return;

    cbData = ((DWORD)wcslen(value) + 1u) * sizeof(WCHAR);
    RegSetValueEx(hKey, name, 0, REG_SZ, (const BYTE *)value, cbData);
    RegCloseKey(hKey);
}

static BOOL OpenPrimaryFileLog(void)
{
    int i;

    if (g_file != INVALID_HANDLE_VALUE)
        return TRUE;

    for (i = 0; i < (int)(sizeof(g_primary_file_paths) / sizeof(g_primary_file_paths[0])); i++) {
        g_file = CreateFile(g_primary_file_paths[i], GENERIC_WRITE, FILE_SHARE_READ, NULL,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_file == INVALID_HANDLE_VALUE)
            continue;

        SetFilePointer(g_file, 0, NULL, FILE_END);
        CopyWide(g_file_path, BEDIAG_MAX_PATH_LEN, g_primary_file_paths[i]);
        PrimeFileSink();
        return TRUE;
    }

    return FALSE;
}

static void MaybeRetrySerial(void)
{
    if (g_serial != INVALID_HANDLE_VALUE || g_serial_retry_done)
        return;
    g_serial_retry_done = TRUE;
    OpenSerialLog();
}

static void BuildFileCandidate(const WCHAR *root, DWORD tick, int suffix, WCHAR *out_path, DWORD out_cch)
{
    if (!out_path || out_cch == 0)
        return;
    if (root[0] == L'\\' && root[1] == L'\0') {
        if (suffix == 0)
            wsprintf(out_path, L"\\BEDiag_%lu.txt", tick);
        else
            wsprintf(out_path, L"\\BEDiag_%lu_%02d.txt", tick, suffix);
    } else {
        if (suffix == 0)
            wsprintf(out_path, L"%s\\BEDiag_%lu.txt", root, tick);
        else
            wsprintf(out_path, L"%s\\BEDiag_%lu_%02d.txt", root, tick, suffix);
    }
}

static void TryOpenSecondaryFile(void)
{
    int i;
    DWORD tick;

    if (g_file != INVALID_HANDLE_VALUE)
        return;
    if (OpenPrimaryFileLog())
        return;

    tick = g_driver.init_tick ? g_driver.init_tick : GetTickCount();
    for (i = 0; i < (int)(sizeof(g_file_roots) / sizeof(g_file_roots[0])); i++) {
        int suffix;
        const WCHAR *root;
        root = g_file_roots[i];
        if (!(root[0] == L'\\' && root[1] == L'\0') && !DirectoryExists(root))
            continue;

        for (suffix = 0; suffix < 16; suffix++) {
            WCHAR candidate[BEDIAG_MAX_PATH_LEN];

            BuildFileCandidate(root, tick, suffix, candidate, BEDIAG_MAX_PATH_LEN);
            g_file = CreateFile(candidate, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
            if (g_file == INVALID_HANDLE_VALUE)
                continue;

            CopyWide(g_file_path, BEDIAG_MAX_PATH_LEN, candidate);
            PrimeFileSink();
            return;
        }
    }
}

static void MaybeReportSerialFailure(void)
{
    if (g_serial_failure_reported)
        return;
    if (g_serial != INVALID_HANDLE_VALUE)
        return;
    if (g_file == INVALID_HANDLE_VALUE)
        return;
    g_serial_failure_reported = TRUE;
    Logf("[BEDIAG] serial_open_failed err=%lu retried=%u\r\n",
         g_serial_open_error,
         g_serial_retry_done ? 1u : 0u);
}

static void LogStage(const char *stage, const char *status)
{
    char active_key_a[256];

    WideToAnsi(g_driver.active_key, active_key_a, sizeof(active_key_a));
    Logf("[BEDIAG_STAGE] build=%s stage=%s tick_ms=%lu context=0x%08lX active_key=\"%s\"",
         BEDIAG_BUILD_TAG,
         stage,
         GetTickCount(),
         g_driver.init_context,
         active_key_a[0] ? active_key_a : "<unavailable>");
    if (status)
        Logf(" status=%s", status);
    if (g_serial == INVALID_HANDLE_VALUE)
        Logf(" serial_open_failed=%lu retried=%u",
             g_serial_open_error,
             g_serial_retry_done ? 1u : 0u);
    else
        Logf(" serial_status=OPEN");
    Logf("\r\n");
    FlushSinks();
}

static BOOL ReadPhysicalBytes(DWORD phys_addr, BYTE *out, DWORD size)
{
    DWORD done;
    BOOL ok;

    done = 0;
    ok = TRUE;
    while (done < size) {
        DWORD cur;
        DWORD page_base;
        DWORD page_off;
        DWORD chunk;
        BYTE *vptr;

        cur = phys_addr + done;
        page_base = cur & ~0xFFFu;
        page_off = cur & 0xFFFu;
        chunk = size - done;
        if (chunk > (0x1000u - page_off))
            chunk = 0x1000u - page_off;

        vptr = (BYTE *)VirtualAlloc(0, 0x1000, MEM_RESERVE, PAGE_NOACCESS);
        if (!vptr) {
            memset(out + done, 0, chunk);
            g_had_error = TRUE;
            ok = FALSE;
            done += chunk;
            continue;
        }

        if (!VirtualCopy(vptr, (LPVOID)(page_base >> 8), 0x1000,
                         PAGE_READONLY | PAGE_NOCACHE | PAGE_PHYSICAL)) {
            memset(out + done, 0, chunk);
            g_had_error = TRUE;
            ok = FALSE;
            VirtualFree(vptr, 0, MEM_RELEASE);
            done += chunk;
            continue;
        }

        __try {
            memcpy(out + done, vptr + page_off, chunk);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            DWORD copied;
            copied = 0;
            while (copied < chunk) {
                __try {
                    out[done + copied] = *((volatile BYTE *)(vptr + page_off + copied));
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    memset(out + done + copied, 0, chunk - copied);
                    g_had_error = TRUE;
                    ok = FALSE;
                    break;
                }
                copied++;
            }
        }

        VirtualFree(vptr, 0, MEM_RELEASE);
        done += chunk;
    }

    return ok;
}

static BOOL SafeReadPhysicalBytes(DWORD phys_addr, BYTE *out, DWORD size)
{
    BOOL ok;
    ok = FALSE;
    __try {
        ok = ReadPhysicalBytes(phys_addr, out, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(out, 0, size);
        g_had_error = TRUE;
        ok = FALSE;
    }
    return ok;
}

static BOOL ReadVirtualBytes(DWORD virt_addr, BYTE *out, DWORD size, DWORD *valid_prefix)
{
    DWORD i;

    if (valid_prefix)
        *valid_prefix = 0;
    if (!out || size == 0)
        return FALSE;

    for (i = 0; i < size; i++) {
        __try {
            out[i] = *((volatile BYTE *)(virt_addr + i));
            if (valid_prefix)
                *valid_prefix = i + 1u;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(out + i, 0, size - i);
            g_had_error = TRUE;
            return FALSE;
        }
    }

    return TRUE;
}

static BOOL SafeReadVirtualBytes(DWORD virt_addr, BYTE *out, DWORD size, DWORD *valid_prefix)
{
    BOOL ok;

    ok = FALSE;
    __try {
        ok = ReadVirtualBytes(virt_addr, out, size, valid_prefix);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(out, 0, size);
        if (valid_prefix)
            *valid_prefix = 0;
        g_had_error = TRUE;
        ok = FALSE;
    }
    return ok;
}

static void BeginSection(const char *title)
{
    TryOpenSecondaryFile();
    MaybeReportSerialFailure();
    Logf("\r\n--- %s ---\r\n", title);
    FlushSinks();
}

static void FormatHexBytes(const BYTE *data, DWORD size, char *dst, DWORD dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    DWORD i;
    DWORD out = 0;

    if (!dst || dst_size == 0)
        return;
    dst[0] = '\0';
    if (!data)
        return;

    for (i = 0; i < size && out + 2 < dst_size; i++) {
        BYTE b = data[i];
        dst[out++] = hex[(b >> 4) & 0x0F];
        dst[out++] = hex[b & 0x0F];
    }
    dst[out] = '\0';
}

static const char *ChangeLabel(snapshot_phase_t phase, BOOL cur_ok, BOOL prev_ok, const BYTE *cur, const BYTE *prev, DWORD size)
{
    if (phase == PHASE_INIT)
        return "INITIAL";
    if (!cur_ok || !prev_ok)
        return "UNAVAILABLE";
    if (memcmp(cur, prev, size) == 0)
        return "NO";
    return "YES";
}

static void LogCaptureBegin(const char *kind, snapshot_phase_t phase,
                            const char *name, DWORD base, DWORD size)
{
    Logf("[CAPTURE_BEGIN] phase=%s kind=%s name=%s addr=0x%08X size=0x%04X\r\n",
         g_phase_names[phase],
         kind,
         name,
         base,
         size);
    FlushSinks();
}

static void CapturePhase(snapshot_phase_t phase)
{
    int i;
    for (i = 0; i < (int)(sizeof(g_regions) / sizeof(g_regions[0])); i++) {
        bediag_region_t *region;
        const char *changed;

        region = &g_regions[i];
        LogCaptureBegin("PA", phase, region->name, region->base, region->size);
        region->ok[phase] = SafeReadPhysicalBytes(region->base, region->data[phase], region->size);
        region->crc[phase] = Crc32(region->data[phase], region->size);
        changed = ChangeLabel(phase,
                              region->ok[phase],
                              phase == PHASE_INIT ? FALSE : region->ok[phase - 1],
                              region->data[phase],
                              phase == PHASE_INIT ? NULL : region->data[phase - 1],
                              region->size);

        Logf("[REGION] phase=%s name=%s PA=0x%08X size=0x%04X status=%s crc32=0x%08X changed_vs_prev=%s\r\n",
             g_phase_names[phase],
             region->name,
             region->base,
             region->size,
             region->ok[phase] ? "OK" : "PARTIAL",
             region->crc[phase],
             changed);

        if (region->emit_raw && region->ok[phase]) {
            DWORD off;
            for (off = 0; off < region->size; off += BEDIAG_RAW_CHUNK_SIZE) {
                DWORD chunk = region->size - off;
                char hex[(BEDIAG_RAW_CHUNK_SIZE * 2u) + 1u];
                if (chunk > BEDIAG_RAW_CHUNK_SIZE)
                    chunk = BEDIAG_RAW_CHUNK_SIZE;
                FormatHexBytes(region->data[phase] + off, chunk, hex, sizeof(hex));
                Logf("[REGION_RAW] phase=%s name=%s PA=0x%08X off=0x%04X size=0x%02X data=%s\r\n",
                     g_phase_names[phase],
                     region->name,
                     region->base,
                     off,
                     chunk,
                     hex);
            }
        }
    }
    FlushSinks();
}

static void CaptureVirtualPhase(snapshot_phase_t phase)
{
    int i;
    for (i = 0; i < (int)(sizeof(g_vregions) / sizeof(g_vregions[0])); i++) {
        bediag_vregion_t *region;
        const char *changed;

        region = &g_vregions[i];
        LogCaptureBegin("VA", phase, region->name, region->base, region->size);
        region->ok[phase] = SafeReadVirtualBytes(region->base, region->data[phase], region->size,
                                                 &region->valid_prefix[phase]);
        region->crc[phase] = Crc32(region->data[phase], region->size);
        changed = ChangeLabel(phase,
                              region->ok[phase],
                              phase == PHASE_INIT ? FALSE : region->ok[phase - 1],
                              region->data[phase],
                              phase == PHASE_INIT ? NULL : region->data[phase - 1],
                              region->size);

        Logf("[VREGION] phase=%s name=%s VA=0x%08X size=0x%04X status=%s valid_prefix=0x%04X crc32=0x%08X changed_vs_prev=%s\r\n",
             g_phase_names[phase],
             region->name,
             region->base,
             region->size,
             region->ok[phase] ? "OK" : "PARTIAL",
             region->valid_prefix[phase],
             region->crc[phase],
             changed);

        if (region->emit_raw && region->valid_prefix[phase] != 0u) {
            DWORD off;
            for (off = 0; off < region->valid_prefix[phase]; off += BEDIAG_RAW_CHUNK_SIZE) {
                DWORD chunk = region->valid_prefix[phase] - off;
                char hex[(BEDIAG_RAW_CHUNK_SIZE * 2u) + 1u];
                if (chunk > BEDIAG_RAW_CHUNK_SIZE)
                    chunk = BEDIAG_RAW_CHUNK_SIZE;
                FormatHexBytes(region->data[phase] + off, chunk, hex, sizeof(hex));
                Logf("[VREGION_RAW] phase=%s name=%s VA=0x%08X off=0x%04X size=0x%02X data=%s\r\n",
                     g_phase_names[phase],
                     region->name,
                     region->base,
                     off,
                     chunk,
                     hex);
            }
        }
    }
    FlushSinks();
}

static BOOL GetSnapshotWord(snapshot_phase_t phase, DWORD pa, DWORD *out)
{
    int i;
    for (i = 0; i < (int)(sizeof(g_regions) / sizeof(g_regions[0])); i++) {
        bediag_region_t *region;
        region = &g_regions[i];
        if (pa < region->base || pa + 4u > region->base + region->size)
            continue;
        if (!region->ok[phase])
            return FALSE;
        *out = ReadLE32(region->data[phase] + (pa - region->base));
        return TRUE;
    }
    return FALSE;
}

static BOOL GetVirtualSnapshotWord(snapshot_phase_t phase, DWORD va, DWORD *out)
{
    int i;
    for (i = 0; i < (int)(sizeof(g_vregions) / sizeof(g_vregions[0])); i++) {
        bediag_vregion_t *region;
        region = &g_vregions[i];
        if (va < region->base || va + 4u > region->base + region->size)
            continue;
        if (!region->ok[phase] &&
            (va - region->base + 4u) > region->valid_prefix[phase])
            return FALSE;
        *out = ReadLE32(region->data[phase] + (va - region->base));
        return TRUE;
    }
    return FALSE;
}

static void EmitFocusWords(snapshot_phase_t phase)
{
    int i;
    for (i = 0; i < (int)(sizeof(g_focus_words) / sizeof(g_focus_words[0])); i++) {
        DWORD value;
        BOOL ok;

        ok = GetSnapshotWord(phase, g_focus_words[i].pa, &value);
        if (!ok) {
            Logf("[FOCUS] phase=%s label=%s PA=0x%08X status=UNREADABLE\r\n",
                 g_phase_names[phase],
                 g_focus_words[i].label,
                 g_focus_words[i].pa);
            continue;
        }

        Logf("[FOCUS] phase=%s label=%s PA=0x%08X status=OK value=0x%08X\r\n",
             g_phase_names[phase],
             g_focus_words[i].label,
             g_focus_words[i].pa,
             value);
    }
    FlushSinks();
}

static void EmitVirtualFocusWords(snapshot_phase_t phase)
{
    int i;
    for (i = 0; i < (int)(sizeof(g_virtual_focus_words) / sizeof(g_virtual_focus_words[0])); i++) {
        DWORD value;
        BOOL ok;

        ok = GetVirtualSnapshotWord(phase, g_virtual_focus_words[i].pa, &value);
        if (!ok) {
            Logf("[VFOCUS] phase=%s label=%s VA=0x%08X status=UNREADABLE\r\n",
                 g_phase_names[phase],
                 g_virtual_focus_words[i].label,
                 g_virtual_focus_words[i].pa);
            continue;
        }

        Logf("[VFOCUS] phase=%s label=%s VA=0x%08X status=OK value=0x%08X\r\n",
             g_phase_names[phase],
             g_virtual_focus_words[i].label,
             g_virtual_focus_words[i].pa,
             value);
    }
    FlushSinks();
}

static void LogValueData(DWORD type, const BYTE *data, DWORD cb_data)
{
    if (type == REG_DWORD && cb_data >= sizeof(DWORD)) {
        DWORD v;
        v = *((const DWORD *)data);
        Logf("DWORD(0x%08X)", v);
        return;
    }

    if ((type == REG_SZ || type == REG_EXPAND_SZ) && cb_data >= sizeof(WCHAR)) {
        char text[256];
        WideBytesToAnsi((const WCHAR *)data, cb_data / sizeof(WCHAR), text, sizeof(text));
        Logf("STR(\"%s\")", text);
        return;
    }

    if (type == REG_MULTI_SZ && cb_data >= sizeof(WCHAR)) {
        const WCHAR *cursor;
        DWORD remain;

        cursor = (const WCHAR *)data;
        remain = cb_data / sizeof(WCHAR);
        Logf("MULTI(\"");
        while (remain > 0 && *cursor != L'\0') {
            char text[128];
            DWORD seg_len;

            seg_len = 0;
            while (seg_len < remain && cursor[seg_len] != L'\0')
                seg_len++;
            WideBytesToAnsi(cursor, seg_len, text, sizeof(text));
            Logf("%s|", text);
            if (seg_len >= remain)
                break;
            cursor += seg_len + 1;
            if (remain < seg_len + 1)
                break;
            remain -= seg_len + 1;
        }
        Logf("\")");
        return;
    }

    Logf("TYPE=%lu SIZE=%lu", type, cb_data);
}

static void DumpKeyValues(snapshot_phase_t phase, const char *path_a, HKEY hKey)
{
    DWORD vIndex;
    vIndex = 0;
    while (1) {
        WCHAR value_name[128];
        DWORD value_name_len;
        BYTE data[256];
        DWORD data_len;
        DWORD type;
        LONG rc;
        char value_name_a[128];

        value_name_len = sizeof(value_name) / sizeof(value_name[0]);
        data_len = sizeof(data);
        type = 0;
        rc = RegEnumValue(hKey, vIndex, value_name, &value_name_len, NULL, &type, data, &data_len);
        if (rc == ERROR_NO_MORE_ITEMS)
            break;
        if (rc != ERROR_SUCCESS) {
            Logf("[REG_VALUE] phase=%s path=\"%s\" enum_error=%ld index=%lu\r\n",
                 g_phase_names[phase], path_a, rc, vIndex);
            break;
        }

        if (value_name_len == 0)
            CopyAnsi(value_name_a, sizeof(value_name_a), "(Default)");
        else {
            if (value_name_len >= sizeof(value_name) / sizeof(value_name[0]))
                value_name_len = (sizeof(value_name) / sizeof(value_name[0])) - 1;
            value_name[value_name_len] = L'\0';
            WideBytesToAnsi(value_name, value_name_len, value_name_a, sizeof(value_name_a));
        }

        Logf("[REG_VALUE] phase=%s path=\"%s\" name=\"%s\" data=",
             g_phase_names[phase], path_a, value_name_a);
        LogValueData(type, data, data_len);
        Logf("\r\n");
        vIndex++;
    }
}

static void DumpRegistryKeyShallow(snapshot_phase_t phase, const WCHAR *root_path)
{
    HKEY hRoot;
    LONG open_rc;
    char root_a[256];
    DWORD kIndex;

    hRoot = NULL;
    WideToAnsi(root_path, root_a, sizeof(root_a));
    open_rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, root_path, 0, KEY_READ, &hRoot);
    if (open_rc != ERROR_SUCCESS) {
        Logf("[REG_KEY] phase=%s path=\"HKLM\\%s\" open_failed=%ld\r\n",
             g_phase_names[phase], root_a, open_rc);
        return;
    }

    Logf("[REG_KEY] phase=%s path=\"HKLM\\%s\" status=OK\r\n",
         g_phase_names[phase], root_a);
    DumpKeyValues(phase, root_a, hRoot);

    kIndex = 0;
    while (1) {
        WCHAR sub_name[128];
        DWORD sub_name_len;
        FILETIME ft;
        LONG rc;
        WCHAR full_path[256];
        HKEY hSub;
        char full_path_a[256];

        sub_name_len = sizeof(sub_name) / sizeof(sub_name[0]);
        rc = RegEnumKeyEx(hRoot, kIndex, sub_name, &sub_name_len, NULL, NULL, NULL, &ft);
        if (rc == ERROR_NO_MORE_ITEMS)
            break;
        if (rc != ERROR_SUCCESS) {
            Logf("[REG_SUBKEY] phase=%s parent=\"HKLM\\%s\" enum_error=%ld index=%lu\r\n",
                 g_phase_names[phase], root_a, rc, kIndex);
            break;
        }

        if (sub_name_len >= sizeof(sub_name) / sizeof(sub_name[0]))
            sub_name_len = (sizeof(sub_name) / sizeof(sub_name[0])) - 1;
        sub_name[sub_name_len] = L'\0';
        wsprintf(full_path, L"%s\\%s", root_path, sub_name);
        WideToAnsi(full_path, full_path_a, sizeof(full_path_a));
        Logf("[REG_SUBKEY] phase=%s path=\"HKLM\\%s\"\r\n",
             g_phase_names[phase], full_path_a);

        hSub = NULL;
        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, full_path, 0, KEY_READ, &hSub) == ERROR_SUCCESS) {
            DumpKeyValues(phase, full_path_a, hSub);
            RegCloseKey(hSub);
        }

        kIndex++;
    }

    RegCloseKey(hRoot);
}

static void CaptureRegistryState(snapshot_phase_t phase)
{
    DumpRegistryKeyShallow(phase, L"Drivers\\Active");
    DumpRegistryKeyShallow(phase, L"Drivers\\BuiltIn");
    DumpRegistryKeyShallow(phase, L"System\\StorageManager\\Profiles");
    DumpRegistryKeyShallow(phase, L"System\\StorageManager\\AutoLoad");
    FlushSinks();
}

static void LogDriverState(void)
{
    char active_key_a[256];
    char file_path_a[BEDIAG_MAX_PATH_LEN];

    WideToAnsi(g_driver.active_key, active_key_a, sizeof(active_key_a));
    WideToAnsi(g_file_path, file_path_a, sizeof(file_path_a));

    Logf("build=%s\r\n", BEDIAG_BUILD_TAG);
    Logf("tick_ms=%lu\r\n", GetTickCount());
    Logf("init_tick_ms=%lu\r\n", g_driver.init_tick);
    Logf("worker_started=%u worker_thread_id=%lu stop_requested=%ld\r\n",
         g_driver.worker_started ? 1u : 0u,
         g_driver.worker_thread_id,
         g_driver.stop_requested);
    Logf("active_key=\"%s\"\r\n", active_key_a[0] ? active_key_a : "<unavailable>");
    Logf("serial_status=%s serial_open_error=%lu serial_retried=%u\r\n",
         g_serial != INVALID_HANDLE_VALUE ? "OPEN" : "UNAVAILABLE",
         g_serial_open_error,
         g_serial_retry_done ? 1u : 0u);
    Logf("file_status=%s file_path=\"%s\"\r\n",
         g_file != INVALID_HANDLE_VALUE ? "OPEN" : "UNAVAILABLE",
         file_path_a[0] ? file_path_a : "<unavailable>");
    Logf("backlog_used=%lu backlog_overflow=%u\r\n",
         g_backlog_used,
         g_backlog_overflow ? 1u : 0u);
    FlushSinks();
}

static void CopyContextPath(DWORD dwContext, WCHAR *dst, DWORD dst_cch)
{
    DWORD i;
    const WCHAR *src;

    CopyWide(dst, dst_cch, L"");
    src = (const WCHAR *)dwContext;
    if (!src)
        return;

    __try {
        for (i = 0; i + 1 < dst_cch; i++) {
            WCHAR ch;
            ch = src[i];
            dst[i] = ch;
            if (ch == L'\0')
                return;
        }
        dst[dst_cch - 1] = L'\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        CopyWide(dst, dst_cch, L"<unreadable>");
    }
}

static void UpdateDriverContext(bediag_driver_t *driver, DWORD dwContext)
{
    if (!driver || dwContext == 0)
        return;

    driver->init_context = dwContext;
    CopyContextPath(dwContext, driver->active_key,
                    sizeof(driver->active_key) / sizeof(driver->active_key[0]));
}

static void LogDuplicateInit(DWORD dwContext)
{
    WCHAR incoming_key[256];
    char incoming_key_a[256];
    char active_key_a[256];

    CopyContextPath(dwContext, incoming_key,
                    sizeof(incoming_key) / sizeof(incoming_key[0]));
    WideToAnsi(incoming_key, incoming_key_a, sizeof(incoming_key_a));
    WideToAnsi(g_driver.active_key, active_key_a, sizeof(active_key_a));

    Logf("[BEDIAG_DUPINIT] tick_ms=%lu new_context=0x%08lX refs=%ld worker_started=%u incoming_key=\"%s\" active_key=\"%s\"\r\n",
         GetTickCount(),
         dwContext,
         g_driver_refcount,
         g_driver.worker_started ? 1u : 0u,
         incoming_key_a[0] ? incoming_key_a : "<unavailable>",
         active_key_a[0] ? active_key_a : "<unavailable>");
    FlushSinks();
}

static DWORD WINAPI BEDiagWorkerThread(LPVOID arg)
{
    bediag_driver_t *driver;
    const char *final_status;
    WCHAR status_w[32];

    driver = (bediag_driver_t *)arg;
    driver->worker_started = TRUE;
    driver->worker_thread_id = GetCurrentThreadId();
    SetBreadcrumbDWORD(L"BEDiagWorkerStarted", 1);

    TryOpenSecondaryFile();
    MaybeReportSerialFailure();

    BeginSection("BEDIAG INIT");
    Logf("tick_ms=%lu\r\n", GetTickCount());
    Logf("context=0x%08lX\r\n", driver->init_context);
    {
        char active_key_a[256];
        WideToAnsi(driver->active_key, active_key_a, sizeof(active_key_a));
        Logf("active_key=\"%s\"\r\n", active_key_a[0] ? active_key_a : "<unavailable>");
    }
    FlushSinks();

    BeginSection("SNAPSHOT INIT");
    Logf("tick_ms=%lu phase=%s\r\n", GetTickCount(), g_phase_names[PHASE_INIT]);
    CapturePhase(PHASE_INIT);
    CaptureVirtualPhase(PHASE_INIT);
    DumpTlbPhase(PHASE_INIT);
    EmitFocusWords(PHASE_INIT);
    EmitVirtualFocusWords(PHASE_INIT);
    CaptureRegistryState(PHASE_INIT);
    LogStage("snapshot_init_done", NULL);

    if (!driver->stop_requested)
        Sleep(1000);

    MaybeRetrySerial();
    TryOpenSecondaryFile();
    MaybeReportSerialFailure();

    if (!driver->stop_requested) {
        BeginSection("SNAPSHOT +1S");
        Logf("tick_ms=%lu phase=%s\r\n", GetTickCount(), g_phase_names[PHASE_PLUS1S]);
        CapturePhase(PHASE_PLUS1S);
        CaptureVirtualPhase(PHASE_PLUS1S);
        EmitFocusWords(PHASE_PLUS1S);
        EmitVirtualFocusWords(PHASE_PLUS1S);
        CaptureRegistryState(PHASE_PLUS1S);
        LogStage("snapshot_1s_done", NULL);
    }

    if (!driver->stop_requested)
        Sleep(4000);

    TryOpenSecondaryFile();
    MaybeReportSerialFailure();

    if (!driver->stop_requested) {
        BeginSection("SNAPSHOT +5S");
        Logf("tick_ms=%lu phase=%s\r\n", GetTickCount(), g_phase_names[PHASE_PLUS5S]);
        CapturePhase(PHASE_PLUS5S);
        CaptureVirtualPhase(PHASE_PLUS5S);
        EmitFocusWords(PHASE_PLUS5S);
        EmitVirtualFocusWords(PHASE_PLUS5S);
        CaptureRegistryState(PHASE_PLUS5S);
        LogStage("snapshot_5s_done", NULL);
    }

    BeginSection("DRIVER STATE");
    LogDriverState();

    BeginSection("BEDIAG DONE");
    Logf("tick_ms=%lu\r\n", GetTickCount());
    final_status = driver->stop_requested ? "ABORTED" : (g_had_error ? "PARTIAL_ERROR" : "OK");
    Logf("status=%s\r\n", final_status);
    status_w[0] = L'\0';
    if (MultiByteToWideChar(CP_ACP, 0, final_status, -1, status_w, sizeof(status_w) / sizeof(status_w[0])) > 0)
        SetBreadcrumbString(L"BEDiagLastStatus", status_w);
    LogStage("done", final_status);
    FlushSinks();

    return 0;
}

static void CloseLogs(void)
{
    if (g_serial != INVALID_HANDLE_VALUE) {
        CloseHandle(g_serial);
        g_serial = INVALID_HANDLE_VALUE;
    }
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

extern "C" BOOL WINAPI DllMain(HANDLE hInst, DWORD reason, LPVOID reserved)
{
    (void)hInst;
    (void)reason;
    (void)reserved;
    return TRUE;
}

extern "C" DWORD WINAPI BDG_Init(DWORD dwContext)
{
    if (InterlockedCompareExchange(&g_driver_active, 1, 0) != 0) {
        InterlockedIncrement(&g_driver_refcount);
        UpdateDriverContext(&g_driver, dwContext);
        OpenPrimaryFileLog();
        TryOpenSecondaryFile();
        MaybeReportSerialFailure();
        LogDuplicateInit(dwContext);
        return (DWORD)&g_driver;
    }

    memset(&g_driver, 0, sizeof(g_driver));
    memset(g_file_path, 0, sizeof(g_file_path));
    memset(g_backlog, 0, sizeof(g_backlog));
    g_backlog_used = 0;
    g_backlog_overflow = FALSE;
    g_had_error = FALSE;
    g_serial_open_error = 0;
    g_serial_retry_done = FALSE;
    g_serial_failure_reported = FALSE;
    g_driver.init_tick = GetTickCount();
    UpdateDriverContext(&g_driver, dwContext);
    InterlockedExchange(&g_driver_refcount, 1);

    OpenPrimaryFileLog();
    SetBreadcrumbDWORD(L"BEDiagLoaded", 1);
    SetBreadcrumbDWORD(L"BEDiagInitTick", g_driver.init_tick);
    SetBreadcrumbDWORD(L"BEDiagWorkerStarted", 0);
    LogStage("init_enter", NULL);

    g_driver.worker_thread = CreateThread(NULL, 0, BEDiagWorkerThread, &g_driver, 0, &g_driver.worker_thread_id);
    if (!g_driver.worker_thread) {
        TryOpenSecondaryFile();
        MaybeReportSerialFailure();
        BeginSection("BEDIAG INIT");
        Logf("tick_ms=%lu\r\n", GetTickCount());
        Logf("worker_start_failed err=%lu\r\n", GetLastError());
        BeginSection("BEDIAG DONE");
        Logf("status=FAILED\r\n");
        SetBreadcrumbString(L"BEDiagLastStatus", L"FAILED");
        LogStage("done", "FAILED");
        FlushSinks();
        g_had_error = TRUE;
        InterlockedExchange(&g_driver_refcount, 0);
        InterlockedExchange(&g_driver_active, 0);
        return 0;
    }

    LogStage("worker_created", NULL);
    return (DWORD)&g_driver;
}

extern "C" BOOL WINAPI BDG_Deinit(DWORD hDeviceContext)
{
    bediag_driver_t *driver;
    LONG refs_left;

    driver = (bediag_driver_t *)hDeviceContext;
    if (driver != &g_driver)
        return FALSE;

    refs_left = InterlockedDecrement(&g_driver_refcount);
    if (refs_left > 0) {
        Logf("[BEDIAG_DEINIT] tick_ms=%lu refs_left=%ld status=DEFERRED\r\n",
             GetTickCount(),
             refs_left);
        FlushSinks();
        return TRUE;
    }
    if (refs_left < 0) {
        InterlockedExchange(&g_driver_refcount, 0);
        refs_left = 0;
    }

    InterlockedExchange(&driver->stop_requested, 1);
    if (driver->worker_thread) {
        WaitForSingleObject(driver->worker_thread, 2000);
        CloseHandle(driver->worker_thread);
        driver->worker_thread = NULL;
    }

    CloseLogs();
    InterlockedExchange(&g_driver_active, 0);
    return TRUE;
}

extern "C" DWORD WINAPI BDG_Open(DWORD hDeviceContext, DWORD AccessCode, DWORD ShareMode)
{
    (void)AccessCode;
    (void)ShareMode;
    return hDeviceContext;
}

extern "C" BOOL WINAPI BDG_Close(DWORD hOpenContext)
{
    (void)hOpenContext;
    return TRUE;
}

extern "C" BOOL WINAPI BDG_IOControl(DWORD hOpenContext,
                                      DWORD dwCode,
                                      PBYTE pBufIn,
                                      DWORD dwLenIn,
                                      PBYTE pBufOut,
                                      DWORD dwLenOut,
                                      PDWORD pdwActualOut)
{
    (void)hOpenContext;
    (void)dwCode;
    (void)pBufIn;
    (void)dwLenIn;
    (void)pBufOut;
    (void)dwLenOut;
    if (pdwActualOut)
        *pdwActualOut = 0;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}
