#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

extern "C" BOOL VirtualCopy(LPVOID, LPVOID, DWORD, DWORD);
typedef BOOL (*SETKMODE)(BOOL);
typedef DWORD (*SETPROCPERMISSIONS)(DWORD);

#ifndef PAGE_PHYSICAL
#define PAGE_PHYSICAL 0x40000000
#endif

#ifndef PAGE_NOCACHE
#define PAGE_NOCACHE 0x0200
#endif

#define MAX_REGION_SIZE          0x1000
#define NAND_READ_LOOPS          10
#define STORAGE_READ_LOOPS       10
#define MAX_REG_DUMP_DEPTH       4
#define MAX_DIR_ENTRIES          128
#define UTF16_HINT_PA            0x006794E0u
#define UTF16_HINT_SIZE          0x40u

enum snapshot_phase_t {
    SNAP_EARLY = 0,
    SNAP_SETTLE = 1,
    SNAP_POST = 2,
    SNAP_COUNT = 3
};

typedef struct {
    const char *name;
    DWORD base;
    DWORD size;
    BOOL ok[SNAP_COUNT];
    BYTE data[SNAP_COUNT][MAX_REGION_SIZE];
} probe_region_t;

typedef struct {
    DWORD pa;
    const char *label;
    const char *alias;
} focus_word_t;

static const char *g_phase_names[SNAP_COUNT] = {
    "early",
    "settle",
    "post"
};

static HANDLE g_hFile = INVALID_HANDLE_VALUE;
static BOOL g_had_error = FALSE;
static char g_boot_tag[64] = "Unknown";

static probe_region_t g_regions[] = {
    { "kdata_page",                0x00002000, 0x1000, {FALSE, FALSE, FALSE}, {{0}, {0}, {0}} },
    { "resume_context_2200",       0x00002200, 0x0100, {FALSE, FALSE, FALSE}, {{0}, {0}, {0}} },
    { "stack_frame_1700",          0x00001700, 0x0100, {FALSE, FALSE, FALSE}, {{0}, {0}, {0}} },
    { "bootctx_alias_A0006000",    0x00006000, 0x1000, {FALSE, FALSE, FALSE}, {{0}, {0}, {0}} },
    { "bootparam_alias_A001D000",  0x0001D000, 0x1000, {FALSE, FALSE, FALSE}, {{0}, {0}, {0}} },
    { "bootparam_alias_A002D000",  0x0002D000, 0x1000, {FALSE, FALSE, FALSE}, {{0}, {0}, {0}} },
    { "callback_table_51600",      0x00051600, 0x0200, {FALSE, FALSE, FALSE}, {{0}, {0}, {0}} },
    { "objptr_table_80660000",     0x00660000, 0x1000, {FALSE, FALSE, FALSE}, {{0}, {0}, {0}} },
    { "obj_table_8066BF00",        0x0066BF00, 0x0200, {FALSE, FALSE, FALSE}, {{0}, {0}, {0}} },
    { "callback_globals_80679400", 0x00679400, 0x0200, {FALSE, FALSE, FALSE}, {{0}, {0}, {0}} },
    { "obj_table_8067BF00",        0x0067BF00, 0x0200, {FALSE, FALSE, FALSE}, {{0}, {0}, {0}} },
    { "vrc4173_base",              0x0A000000, 0x1000, {FALSE, FALSE, FALSE}, {{0}, {0}, {0}} },
    { "vrc4173_page_A000",         0x0A00A000, 0x1000, {FALSE, FALSE, FALSE}, {{0}, {0}, {0}} },
    { "vrc4173_page_C000",         0x0A00C000, 0x1000, {FALSE, FALSE, FALSE}, {{0}, {0}, {0}} },
    { "vr4131_core_page",          0x0F000000, 0x1000, {FALSE, FALSE, FALSE}, {{0}, {0}, {0}} }
};

static const focus_word_t g_focus_words[] = {
    { 0x0A000C38, "nand_sideband_c38",        NULL },
    { 0x0A000C30, "nand_c30",                 "0xAA000C30" },
    { 0x0A000C34, "nand_c34",                 "0xAA000C34" },
    { 0x0A000C3C, "nand_c3c",                 "0xAA000C3C" },
    { 0x0A000C48, "nand_c48",                 "0xAA000C48" },
    { 0x0A000C4C, "nand_c4c",                 "0xAA000C4C" },
    { 0x0F000080, "icu_0080",                 "0xAF000080" },
    { 0x0F00008C, "icu_008c",                 "0xAF00008C" },
    { 0x0F000100, "pmu_0100",                 "0xAF000100" },
    { 0x0F000104, "pmu_0104",                 "0xAF000104" },
    { 0x0F000110, "pmu_0110",                 "0xAF000110" },
    { 0x0F000114, "pmu_0114",                 "0xAF000114" },
    { 0x00002220, "ctx_2220",                 "0xA0002220" },
    { 0x00002228, "ctx_2228",                 "0xA0002228" },
    { 0x0000222C, "ctx_222C",                 "0xA000222C" },
    { 0x00002270, "ctx_2270",                 "0xA0002270" },
    { 0x00002274, "ctx_2274",                 "0xA0002274" },
    { 0x000022D0, "ctx_22D0",                 "0xA00022D0" },
    { 0x00001760, "stack_1760",               "0x80001760" },
    { 0x00001764, "stack_1764",               "0x80001764" },
    { 0x00001788, "stack_1788",               "0x80001788" },
    { 0x0000179C, "stack_179C",               "0x8000179C" },
    { 0x000017B4, "stack_17B4",               "0x800017B4" },
    { 0x006794EC, "g94EC",                    "0x806794EC" },
    { 0x006794F0, "g94F0",                    "0x806794F0" },
    { 0x00679508, "g9508",                    "0x80679508" },
    { 0x00679510, "g9510",                    "0x80679510" }
};

static void Log(const char *fmt, ...)
{
    if (g_hFile == INVALID_HANDLE_VALUE)
        return;

    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    {
        int n = _vsnprintf(buffer, sizeof(buffer) - 1, fmt, args);
        if (n < 0)
            buffer[sizeof(buffer) - 1] = '\0';
        else
            buffer[n] = '\0';
    }
    va_end(args);

    {
        DWORD written = 0;
        WriteFile(g_hFile, buffer, (DWORD)strlen(buffer), &written, NULL);
    }
}

static void CopyWide(WCHAR *dst, DWORD dst_cch, const WCHAR *src)
{
    DWORD i = 0;
    if (!dst || dst_cch == 0)
        return;

    dst[0] = L'\0';
    if (!src)
        return;

    while (i + 1 < dst_cch && src[i] != L'\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = L'\0';
}

static void CopyAnsi(char *dst, DWORD dst_cch, const char *src)
{
    DWORD i = 0;
    if (!dst || dst_cch == 0)
        return;

    dst[0] = '\0';
    if (!src)
        return;

    while (i + 1 < dst_cch && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void WideToAnsi(const WCHAR *src, char *dst, int dst_size)
{
    if (!dst || dst_size <= 0)
        return;
    dst[0] = '\0';
    if (!src)
        return;

    {
        int ok = WideCharToMultiByte(CP_ACP, 0, src, -1, dst, dst_size, NULL, NULL);
        if (ok <= 0)
            _snprintf(dst, dst_size - 1, "<conv-failed>");
    }
    dst[dst_size - 1] = '\0';
}

static void WideBytesToAnsi(const WCHAR *src, DWORD src_wchars_max, char *dst, int dst_size, BOOL *truncated)
{
    DWORD wlen = 0;
    int out = 0;

    if (truncated)
        *truncated = FALSE;

    if (!dst || dst_size <= 0) {
        return;
    }
    dst[0] = '\0';

    if (!src || src_wchars_max == 0)
        return;

    while (wlen < src_wchars_max && src[wlen] != L'\0')
        wlen++;

    if (wlen == src_wchars_max && truncated)
        *truncated = TRUE;

    out = WideCharToMultiByte(CP_ACP, 0, src, (int)wlen, dst, dst_size - 1, NULL, NULL);
    if (out <= 0) {
        _snprintf(dst, dst_size - 1, "<conv-failed>");
        dst[dst_size - 1] = '\0';
        return;
    }
    dst[out] = '\0';
}

static BOOL WideStartsWith(const WCHAR *text, const WCHAR *prefix)
{
    DWORD i = 0;
    if (!text || !prefix)
        return FALSE;

    while (prefix[i] != L'\0') {
        if (text[i] != prefix[i])
            return FALSE;
        i++;
    }
    return TRUE;
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
    DWORD crc = 0xFFFFFFFFu;
    DWORD i = 0;
    for (i = 0; i < size; i++) {
        DWORD j = 0;
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

static void EnableKernelAccess(void)
{
    HMODULE hCore = GetModuleHandle(L"coredll.dll");
    if (!hCore) {
        Log("[PRIV] GetModuleHandle(coredll) failed\r\n");
        g_had_error = TRUE;
        return;
    }

    {
        SETKMODE pSetKMode = (SETKMODE)GetProcAddress(hCore, L"SetKMode");
        if (pSetKMode) {
            BOOL km = pSetKMode(TRUE);
            Log("[PRIV] SetKMode(TRUE) -> %u\r\n", km ? 1u : 0u);
        } else {
            Log("[PRIV] SetKMode not available\r\n");
            g_had_error = TRUE;
        }
    }

    {
        SETPROCPERMISSIONS pSetPerms = (SETPROCPERMISSIONS)GetProcAddress(hCore, L"SetProcPermissions");
        if (pSetPerms) {
            DWORD old_perm = pSetPerms(0xFFFFFFFFu);
            Log("[PRIV] SetProcPermissions(0xFFFFFFFF) old=0x%08X\r\n", old_perm);
        } else {
            Log("[PRIV] SetProcPermissions not available\r\n");
            g_had_error = TRUE;
        }
    }
}

static BOOL ReadPhysicalBytes(DWORD phys_addr, BYTE *out, DWORD size)
{
    DWORD done = 0;
    BOOL ok = TRUE;

    while (done < size) {
        DWORD cur = phys_addr + done;
        DWORD page_base = cur & ~0xFFFu;
        DWORD page_off = cur & 0xFFFu;
        DWORD chunk = size - done;
        BYTE *vptr = NULL;

        if (chunk > (0x1000u - page_off))
            chunk = 0x1000u - page_off;

        vptr = (BYTE *)VirtualAlloc(0, 0x1000, MEM_RESERVE, PAGE_NOACCESS);
        if (!vptr) {
            Log("[MAP_FAIL] PA=0x%08X VirtualAlloc failed err=%lu\r\n", page_base, GetLastError());
            memset(out + done, 0, chunk);
            g_had_error = TRUE;
            ok = FALSE;
            done += chunk;
            continue;
        }

        if (!VirtualCopy(vptr, (LPVOID)(page_base >> 8), 0x1000,
                         PAGE_READONLY | PAGE_NOCACHE | PAGE_PHYSICAL)) {
            Log("[MAP_FAIL] PA=0x%08X VirtualCopy failed err=%lu\r\n", page_base, GetLastError());
            memset(out + done, 0, chunk);
            g_had_error = TRUE;
            ok = FALSE;
            VirtualFree(vptr, 0, MEM_RELEASE);
            done += chunk;
            continue;
        }

        {
            DWORD copied = 0;
            BOOL chunk_fault = FALSE;
            while (copied + 4u <= chunk) {
                DWORD word = 0;
                __try {
                    word = *((volatile DWORD *)(vptr + page_off + copied));
                    out[done + copied + 0] = (BYTE)((word >> 0) & 0xFF);
                    out[done + copied + 1] = (BYTE)((word >> 8) & 0xFF);
                    out[done + copied + 2] = (BYTE)((word >> 16) & 0xFF);
                    out[done + copied + 3] = (BYTE)((word >> 24) & 0xFF);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    Log("[READ_FAULT] PA=0x%08X size=0x%lX code=0x%08lX\r\n",
                        cur + copied, chunk - copied, GetExceptionCode());
                    memset(out + done + copied, 0, chunk - copied);
                    g_had_error = TRUE;
                    ok = FALSE;
                    chunk_fault = TRUE;
                }

                if (chunk_fault)
                    break;
                copied += 4u;
            }

            if (!chunk_fault) {
                while (copied < chunk) {
                    __try {
                        out[done + copied] = *((volatile BYTE *)(vptr + page_off + copied));
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        Log("[READ_FAULT] PA=0x%08X size=0x%lX code=0x%08lX\r\n",
                            cur + copied, chunk - copied, GetExceptionCode());
                        memset(out + done + copied, 0, chunk - copied);
                        g_had_error = TRUE;
                        ok = FALSE;
                        break;
                    }
                    copied++;
                }
            }
        }

        VirtualFree(vptr, 0, MEM_RELEASE);
        done += chunk;
    }

    return ok;
}

static BOOL SafeReadPhysicalBytes(DWORD phys_addr, BYTE *out, DWORD size)
{
    BOOL ok = FALSE;
    __try {
        ok = ReadPhysicalBytes(phys_addr, out, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[READ_FATAL] PA=0x%08X size=0x%lX code=0x%08lX\r\n",
            phys_addr, size, GetExceptionCode());
        memset(out, 0, size);
        g_had_error = TRUE;
        ok = FALSE;
    }
    return ok;
}

static void DumpRegionWords(const probe_region_t *r, snapshot_phase_t phase)
{
    const BYTE *data = r->data[phase];
    DWORD off = 0;
    DWORD crc = Crc32(data, r->size);

    Log("[REGION] phase=%s name=%s PA=0x%08X size=0x%04X status=%s crc32=0x%08X\r\n",
        g_phase_names[phase], r->name, r->base, r->size,
        r->ok[phase] ? "OK" : "PARTIAL", crc);

    while (off + 16u <= r->size) {
        DWORD w0 = ReadLE32(data + off + 0);
        DWORD w1 = ReadLE32(data + off + 4);
        DWORD w2 = ReadLE32(data + off + 8);
        DWORD w3 = ReadLE32(data + off + 12);
        Log("0x%08X: %08X %08X %08X %08X\r\n",
            r->base + off, w0, w1, w2, w3);
        off += 16u;
    }
}

static void CaptureSnapshot(snapshot_phase_t phase)
{
    int i = 0;
    for (i = 0; i < (int)(sizeof(g_regions) / sizeof(g_regions[0])); i++) {
        probe_region_t *r = &g_regions[i];
        BOOL ok = SafeReadPhysicalBytes(r->base, r->data[phase], r->size);
        r->ok[phase] = ok;
        __try {
            DumpRegionWords(r, phase);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[DUMP_FATAL] phase=%s name=%s PA=0x%08X code=0x%08lX\r\n",
                g_phase_names[phase], r->name, r->base, GetExceptionCode());
            g_had_error = TRUE;
        }
    }
}

static BOOL GetSnapshotWord(snapshot_phase_t phase, DWORD pa, DWORD *out)
{
    int i = 0;
    for (i = 0; i < (int)(sizeof(g_regions) / sizeof(g_regions[0])); i++) {
        probe_region_t *r = &g_regions[i];
        if (pa < r->base || pa + 4u > r->base + r->size)
            continue;
        if (!r->ok[phase])
            return FALSE;
        *out = ReadLE32(r->data[phase] + (pa - r->base));
        return TRUE;
    }
    return FALSE;
}

static BOOL GetSnapshotBytes(snapshot_phase_t phase, DWORD pa, BYTE *out, DWORD size)
{
    int i = 0;
    for (i = 0; i < (int)(sizeof(g_regions) / sizeof(g_regions[0])); i++) {
        probe_region_t *r = &g_regions[i];
        if (pa < r->base || pa + size > r->base + r->size)
            continue;
        if (!r->ok[phase])
            return FALSE;
        memcpy(out, r->data[phase] + (pa - r->base), size);
        return TRUE;
    }
    return FALSE;
}

static void EmitFocusSummary(void)
{
    int i = 0;
    for (i = 0; i < (int)(sizeof(g_focus_words) / sizeof(g_focus_words[0])); i++) {
        const focus_word_t *f = &g_focus_words[i];
        DWORD v_early = 0;
        DWORD v_settle = 0;
        DWORD v_post = 0;
        BOOL ok_early = GetSnapshotWord(SNAP_EARLY, f->pa, &v_early);
        BOOL ok_settle = GetSnapshotWord(SNAP_SETTLE, f->pa, &v_settle);
        BOOL ok_post = GetSnapshotWord(SNAP_POST, f->pa, &v_post);

        if (!ok_early || !ok_settle || !ok_post) {
            Log("[FOCUS3] label=%s PA=0x%08X alias=%s status=UNREADABLE early_ok=%u settle_ok=%u post_ok=%u\r\n",
                f->label, f->pa, f->alias ? f->alias : "-",
                ok_early ? 1u : 0u, ok_settle ? 1u : 0u, ok_post ? 1u : 0u);
            continue;
        }

        Log("[FOCUS3] label=%s PA=0x%08X alias=%s early=0x%08X settle=0x%08X post=0x%08X\r\n",
            f->label, f->pa, f->alias ? f->alias : "-", v_early, v_settle, v_post);
    }
}

static void EmitUtf16Hint(snapshot_phase_t phase)
{
    BYTE raw[UTF16_HINT_SIZE];
    WCHAR wide_buf[(UTF16_HINT_SIZE / 2) + 1];
    char text[256];
    BOOL truncated = FALSE;
    DWORD i = 0;

    if (!GetSnapshotBytes(phase, UTF16_HINT_PA, raw, UTF16_HINT_SIZE)) {
        Log("[UTF16_HINT] phase=%s PA=0x%08X size=0x%lX status=UNREADABLE\r\n",
            g_phase_names[phase], UTF16_HINT_PA, (DWORD)UTF16_HINT_SIZE);
        return;
    }

    memset(wide_buf, 0, sizeof(wide_buf));
    for (i = 0; i + 1 < UTF16_HINT_SIZE; i += 2) {
        wide_buf[i / 2] = (WCHAR)(raw[i] | (raw[i + 1] << 8));
    }
    wide_buf[UTF16_HINT_SIZE / 2] = L'\0';

    WideBytesToAnsi(wide_buf, UTF16_HINT_SIZE / 2, text, sizeof(text), &truncated);
    Log("[UTF16_HINT] phase=%s PA=0x%08X size=0x%lX text=\"%s\"%s\r\n",
        g_phase_names[phase], UTF16_HINT_PA, (DWORD)UTF16_HINT_SIZE,
        text, truncated ? " trunc=1" : "");
}

static void EmitPairDiffs(const char *pair_name, snapshot_phase_t from_phase, snapshot_phase_t to_phase)
{
    int i = 0;
    DWORD total_changed = 0;

    for (i = 0; i < (int)(sizeof(g_regions) / sizeof(g_regions[0])); i++) {
        probe_region_t *r = &g_regions[i];
        DWORD changed = 0;
        DWORD off = 0;

        if (!r->ok[from_phase] || !r->ok[to_phase]) {
            Log("[DIFF_REGION] pair=%s name=%s skipped from_ok=%u to_ok=%u\r\n",
                pair_name, r->name,
                r->ok[from_phase] ? 1u : 0u,
                r->ok[to_phase] ? 1u : 0u);
            continue;
        }

        while (off + 4u <= r->size) {
            DWORD before = ReadLE32(r->data[from_phase] + off);
            DWORD after = ReadLE32(r->data[to_phase] + off);
            if (before != after) {
                Log("[DIFF_WORD] pair=%s name=%s PA=0x%08X before=0x%08X after=0x%08X\r\n",
                    pair_name, r->name, r->base + off, before, after);
                changed++;
                total_changed++;
            }
            off += 4u;
        }

        Log("[DIFF_REGION] pair=%s name=%s changed_words=%lu\r\n", pair_name, r->name, changed);
    }

    Log("[DIFF_TOTAL] pair=%s changed_words=%lu\r\n", pair_name, total_changed);
}

static BOOL FileExistsRegular(const WCHAR *path)
{
    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(path, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    {
        BOOL ok = ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0);
        FindClose(h);
        return ok;
    }
}

static BOOL DirectoryExists(const WCHAR *path)
{
    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(path, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    {
        BOOL ok = ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        FindClose(h);
        return ok;
    }
}

static BOOL SelectFirstRegularUnderRoot(const WCHAR *root, WCHAR *out_path, DWORD out_cch)
{
    WCHAR search[MAX_PATH];
    WIN32_FIND_DATA fd;
    HANDLE h = INVALID_HANDLE_VALUE;
    BOOL found = FALSE;

    if (root[0] == L'\\' && root[1] == L'\0')
        CopyWide(search, MAX_PATH, L"\\*");
    else
        wsprintf(search, L"%s\\*", root);

    h = FindFirstFile(search, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;

    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            WCHAR path_buf[MAX_PATH];
            if (root[0] == L'\\' && root[1] == L'\0')
                wsprintf(path_buf, L"\\%s", fd.cFileName);
            else
                wsprintf(path_buf, L"%s\\%s", root, fd.cFileName);
            CopyWide(out_path, out_cch, path_buf);
            found = TRUE;
            break;
        }
    } while (FindNextFile(h, &fd));

    FindClose(h);
    return found;
}

static BOOL SelectNandFile(WCHAR *out_path, DWORD out_cch)
{
    WCHAR exe_path[MAX_PATH];
    DWORD exe_len = GetModuleFileName(NULL, exe_path, MAX_PATH);
    if (exe_len > 0 && exe_len < MAX_PATH && WideStartsWith(exe_path, L"\\Nand Disk\\") && FileExistsRegular(exe_path)) {
        CopyWide(out_path, out_cch, exe_path);
        return TRUE;
    }

    if (!DirectoryExists(L"\\Nand Disk"))
        return FALSE;

    return SelectFirstRegularUnderRoot(L"\\Nand Disk", out_path, out_cch);
}

static BOOL RunFileReadWorkload(const char *section_tag,
                                const WCHAR *path,
                                int loops,
                                const WCHAR *root_label)
{
    BYTE buffer[4096];
    DWORD total_bytes = 0;
    int loop = 0;
    char path_a[MAX_PATH];
    char root_a[MAX_PATH];

    WideToAnsi(path, path_a, sizeof(path_a));
    WideToAnsi(root_label, root_a, sizeof(root_a));
    Log("[%s] root=\"%s\" selected_file=\"%s\" loops=%d\r\n", section_tag, root_a, path_a, loops);

    for (loop = 0; loop < loops; loop++) {
        HANDLE h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD loop_bytes = 0;
        BOOL loop_ok = TRUE;

        if (h == INVALID_HANDLE_VALUE) {
            Log("[%s_LOOP] idx=%d open_failed err=%lu\r\n", section_tag, loop + 1, GetLastError());
            g_had_error = TRUE;
            return FALSE;
        }

        while (1) {
            DWORD got = 0;
            BOOL rd = ReadFile(h, buffer, sizeof(buffer), &got, NULL);
            if (!rd) {
                Log("[%s_LOOP] idx=%d read_failed err=%lu\r\n", section_tag, loop + 1, GetLastError());
                g_had_error = TRUE;
                loop_ok = FALSE;
                break;
            }
            if (got == 0)
                break;
            loop_bytes += got;
        }

        CloseHandle(h);
        total_bytes += loop_bytes;
        Log("[%s_LOOP] idx=%d bytes=%lu status=%s\r\n",
            section_tag, loop + 1, loop_bytes, loop_ok ? "OK" : "ERR");
        if (!loop_ok)
            return FALSE;
    }

    Log("[%s] total_bytes=%lu loops=%d\r\n", section_tag, total_bytes, loops);
    return TRUE;
}

static BOOL RunNandWorkload(void)
{
    WCHAR path[MAX_PATH];
    if (!SelectNandFile(path, MAX_PATH)) {
        Log("[NAND] status=ABSENT\r\n");
        return FALSE;
    }
    return RunFileReadWorkload("NAND", path, NAND_READ_LOOPS, L"\\Nand Disk");
}

static BOOL RunStorageWorkload(void)
{
    static const WCHAR *roots[] = {
        L"\\Storage Card",
        L"\\CF Card",
        L"\\PC Card"
    };
    int i = 0;
    WCHAR path[MAX_PATH];

    for (i = 0; i < (int)(sizeof(roots) / sizeof(roots[0])); i++) {
        if (!DirectoryExists(roots[i]))
            continue;
        if (SelectFirstRegularUnderRoot(roots[i], path, MAX_PATH))
            return RunFileReadWorkload("STORAGE", path, STORAGE_READ_LOOPS, roots[i]);
        {
            char root_a[MAX_PATH];
            WideToAnsi(roots[i], root_a, sizeof(root_a));
            Log("[STORAGE] root_present_no_regular_file=\"%s\"\r\n", root_a);
        }
        return FALSE;
    }

    Log("[STORAGE] status=ABSENT\r\n");
    return FALSE;
}

static void LogValueData(DWORD type, const BYTE *data, DWORD cb_data)
{
    if (type == REG_DWORD && cb_data >= sizeof(DWORD)) {
        DWORD v = *((const DWORD *)data);
        Log("DWORD(0x%08X)", v);
        return;
    }

    if ((type == REG_SZ || type == REG_EXPAND_SZ) && cb_data >= sizeof(WCHAR)) {
        char text[512];
        BOOL trunc = FALSE;
        WideBytesToAnsi((const WCHAR *)data, cb_data / sizeof(WCHAR), text, sizeof(text), &trunc);
        Log("STR(\"%s\"%s)", text, trunc ? ",trunc" : "");
        return;
    }

    if (type == REG_MULTI_SZ && cb_data >= sizeof(WCHAR)) {
        const WCHAR *p = (const WCHAR *)data;
        DWORD remain = cb_data / sizeof(WCHAR);
        char one[256];
        Log("MULTI(\"");
        while (remain > 0 && *p) {
            DWORD seg_len = 0;
            int out = 0;
            while (seg_len < remain && p[seg_len] != L'\0')
                seg_len++;
            out = WideCharToMultiByte(CP_ACP, 0, p, (int)seg_len, one, sizeof(one) - 1, NULL, NULL);
            if (out <= 0) {
                _snprintf(one, sizeof(one) - 1, "<conv-failed>");
                one[sizeof(one) - 1] = '\0';
            } else {
                one[out] = '\0';
            }
            Log("%s%s|", one, (seg_len >= remain) ? "(trunc)" : "");
            if (seg_len >= remain)
                break;
            p += seg_len + 1;
            remain -= seg_len + 1;
        }
        Log("\")");
        return;
    }

    {
        DWORD i = 0;
        Log("TYPE=%lu SIZE=%lu HEX=", type, cb_data);
        for (i = 0; i < cb_data && i < 32u; i++)
            Log("%02X", (unsigned)data[i]);
        if (cb_data > 32u)
            Log("...");
    }
}

static void DumpRegistryTree(const WCHAR *path, int depth)
{
    HKEY hKey = NULL;
    LONG open_rc = 0;
    WCHAR next_path[512];
    char path_a[512];

    if (depth > MAX_REG_DUMP_DEPTH) {
        WideToAnsi(path, path_a, sizeof(path_a));
        Log("[REG_KEY] path=\"HKLM\\%s\" status=DEPTH_LIMIT\r\n", path_a);
        return;
    }

    open_rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &hKey);
    WideToAnsi(path, path_a, sizeof(path_a));
    if (open_rc != ERROR_SUCCESS) {
        Log("[REG_KEY] path=\"HKLM\\%s\" open_failed=%ld\r\n", path_a, open_rc);
        return;
    }

    Log("[REG_KEY] path=\"HKLM\\%s\"\r\n", path_a);

    {
        DWORD vIndex = 0;
        while (1) {
            WCHAR value_name[256];
            DWORD value_name_len = sizeof(value_name) / sizeof(value_name[0]);
            BYTE data[512];
            DWORD data_len = sizeof(data);
            DWORD type = 0;
            LONG rc = RegEnumValue(hKey, vIndex, value_name, &value_name_len, NULL, &type, data, &data_len);
            if (rc == ERROR_NO_MORE_ITEMS)
                break;
            if (rc != ERROR_SUCCESS) {
                Log("[REG_VALUE] enum_error=%ld index=%lu\r\n", rc, vIndex);
                break;
            }

            {
                char value_name_a[256];
                if (value_name_len == 0)
                    CopyAnsi(value_name_a, sizeof(value_name_a), "(Default)");
                else
                    WideToAnsi(value_name, value_name_a, sizeof(value_name_a));
                Log("[REG_VALUE] name=\"%s\" ", value_name_a);
                LogValueData(type, data, data_len);
                Log("\r\n");
            }
            vIndex++;
        }
    }

    {
        DWORD kIndex = 0;
        while (1) {
            WCHAR sub_name[256];
            DWORD sub_name_len = sizeof(sub_name) / sizeof(sub_name[0]);
            FILETIME ft;
            LONG rc = RegEnumKeyEx(hKey, kIndex, sub_name, &sub_name_len, NULL, NULL, NULL, &ft);
            if (rc == ERROR_NO_MORE_ITEMS)
                break;
            if (rc != ERROR_SUCCESS) {
                Log("[REG_SUBKEY] enum_error=%ld index=%lu\r\n", rc, kIndex);
                break;
            }

            wsprintf(next_path, L"%s\\%s", path, sub_name);
            DumpRegistryTree(next_path, depth + 1);
            kIndex++;
        }
    }

    RegCloseKey(hKey);
}

static void DumpDirectoryListing(const WCHAR *root)
{
    WCHAR search[MAX_PATH];
    WIN32_FIND_DATA fd;
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD index = 0;
    char root_a[MAX_PATH];

    if (root[0] == L'\\' && root[1] == L'\0')
        CopyWide(search, MAX_PATH, L"\\*");
    else
        wsprintf(search, L"%s\\*", root);

    WideToAnsi(root, root_a, sizeof(root_a));
    h = FindFirstFile(search, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        Log("[FS_ROOT] path=\"%s\" status=OPEN_FAILED err=%lu\r\n", root_a, GetLastError());
        return;
    }

    Log("[FS_ROOT] path=\"%s\" status=OK\r\n", root_a);
    do {
        char name_a[MAX_PATH];
        WideToAnsi(fd.cFileName, name_a, sizeof(name_a));
        Log("[FS_ENTRY] root=\"%s\" name=\"%s\" type=%s size=%lu\r\n",
            root_a,
            name_a,
            (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "DIR" : "FILE",
            fd.nFileSizeLow);
        index++;
        if (index >= MAX_DIR_ENTRIES) {
            Log("[FS_ROOT] path=\"%s\" status=TRUNCATED count=%lu\r\n", root_a, index);
            break;
        }
    } while (FindNextFile(h, &fd));

    FindClose(h);
}

static void SelectBootTag(void)
{
    int rc = MessageBox(NULL,
                        L"Tag this run:\nYes = Cold battery boot\nNo = Warm retained boot\nCancel = Unknown",
                        L"BE300 Probe v3",
                        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (rc == IDYES)
        CopyAnsi(g_boot_tag, sizeof(g_boot_tag), "Cold battery boot");
    else if (rc == IDNO)
        CopyAnsi(g_boot_tag, sizeof(g_boot_tag), "Warm retained boot");
    else
        CopyAnsi(g_boot_tag, sizeof(g_boot_tag), "Unknown");
}

static void LogRunMetadata(void)
{
    SYSTEM_INFO si;
    OSVERSIONINFO vi;
    WCHAR exe_path[MAX_PATH];
    char exe_path_a[MAX_PATH];
    DWORD exe_len = 0;

    memset(&si, 0, sizeof(si));
    memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);

    GetSystemInfo(&si);
    exe_len = GetModuleFileName(NULL, exe_path, MAX_PATH);
    if (exe_len > 0 && exe_len < MAX_PATH)
        WideToAnsi(exe_path, exe_path_a, sizeof(exe_path_a));
    else
        CopyAnsi(exe_path_a, sizeof(exe_path_a), "<unavailable>");

    Log("BE-300 Post-Boot Probe Utility v3\r\n");
    Log("boot_tag=\"%s\"\r\n", g_boot_tag);
    Log("tick_ms=%lu\r\n", GetTickCount());
    Log("exe_path=\"%s\"\r\n", exe_path_a);
    Log("sysinfo processor_type=%lu level=%u revision=0x%04X page_size=%lu\r\n",
        si.dwProcessorType,
        (unsigned)si.wProcessorLevel,
        (unsigned)si.wProcessorRevision,
        si.dwPageSize);

    if (GetVersionEx(&vi)) {
        Log("osversion major=%lu minor=%lu build=%lu platform=%lu csd=\"",
            vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber, vi.dwPlatformId);
        {
            char csd[128];
            WideToAnsi(vi.szCSDVersion, csd, sizeof(csd));
            Log("%s\"\r\n", csd);
        }
    } else {
        Log("osversion status=UNAVAILABLE err=%lu\r\n", GetLastError());
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nCmdShow)
{
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    SelectBootTag();

    g_hFile = CreateFile(L"\\BE300Probe_v3.txt", GENERIC_WRITE, 0, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_hFile == INVALID_HANDLE_VALUE) {
        MessageBox(NULL, L"Could not create \\BE300Probe_v3.txt", L"BE300 Probe v3", MB_OK | MB_ICONHAND);
        return 1;
    }

    __try {
        Log("--- RUN METADATA ---\r\n");
        LogRunMetadata();
        EnableKernelAccess();

        Log("\r\n--- EARLY SNAPSHOT ---\r\n");
        CaptureSnapshot(SNAP_EARLY);

        Sleep(3000);
        Log("\r\n--- SETTLE SNAPSHOT ---\r\n");
        CaptureSnapshot(SNAP_SETTLE);

        Log("\r\n--- NAND WORKLOAD ---\r\n");
        RunNandWorkload();

        Log("\r\n--- STORAGE WORKLOAD ---\r\n");
        RunStorageWorkload();

        Log("\r\n--- POST SNAPSHOT ---\r\n");
        CaptureSnapshot(SNAP_POST);

        Log("\r\n--- DIFF SUMMARY ---\r\n");
        EmitPairDiffs("early_to_settle", SNAP_EARLY, SNAP_SETTLE);
        EmitPairDiffs("settle_to_post", SNAP_SETTLE, SNAP_POST);
        EmitPairDiffs("early_to_post", SNAP_EARLY, SNAP_POST);
        EmitFocusSummary();
        EmitUtf16Hint(SNAP_EARLY);
        EmitUtf16Hint(SNAP_SETTLE);
        EmitUtf16Hint(SNAP_POST);

        Log("\r\n--- DRIVER INVENTORY ---\r\n");
        DumpRegistryTree(L"Drivers\\Active", 0);
        DumpRegistryTree(L"Drivers\\BuiltIn", 0);

        Log("\r\n--- STORAGE MANAGER ---\r\n");
        DumpRegistryTree(L"System\\StorageManager\\Profiles", 0);
        DumpRegistryTree(L"System\\StorageManager\\AutoLoad", 0);

        Log("\r\n--- FILESYSTEM ROOTS ---\r\n");
        DumpDirectoryListing(L"\\");
        if (DirectoryExists(L"\\Nand Disk"))
            DumpDirectoryListing(L"\\Nand Disk");
        if (DirectoryExists(L"\\Storage Card"))
            DumpDirectoryListing(L"\\Storage Card");
        if (DirectoryExists(L"\\CF Card"))
            DumpDirectoryListing(L"\\CF Card");
        if (DirectoryExists(L"\\PC Card"))
            DumpDirectoryListing(L"\\PC Card");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[FATAL] code=0x%08lX\r\n", GetExceptionCode());
        g_had_error = TRUE;
    }

    Log("\r\nprobe_complete status=%s\r\n", g_had_error ? "PARTIAL_ERROR" : "OK");
    CloseHandle(g_hFile);
    g_hFile = INVALID_HANDLE_VALUE;

    if (g_had_error) {
        MessageBox(NULL,
                   L"BE300Probe v3 completed with partial errors.\nSee \\BE300Probe_v3.txt",
                   L"BE300 Probe v3",
                   MB_OK | MB_ICONEXCLAMATION);
        return 2;
    }

    MessageBox(NULL,
               L"BE300Probe v3 complete.\nSee \\BE300Probe_v3.txt",
               L"BE300 Probe v3",
               MB_OK | MB_ICONASTERISK);
    return 0;
}
