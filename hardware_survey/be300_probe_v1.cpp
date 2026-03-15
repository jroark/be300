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

#define REGION_SNAPSHOT_SIZE 0x1000
#define NAND_READ_LOOPS 10
#define MAX_REG_DUMP_DEPTH 4

typedef struct {
    const char *name;
    DWORD base;
    DWORD size;
    BOOL baseline_ok;
    BOOL post_ok;
    BYTE baseline[REGION_SNAPSHOT_SIZE];
    BYTE post[REGION_SNAPSHOT_SIZE];
} probe_region_t;

typedef struct {
    DWORD pa;
    const char *label;
    const char *alias;
} focus_word_t;

static HANDLE g_hFile = INVALID_HANDLE_VALUE;
static BOOL g_had_error = FALSE;

static probe_region_t g_regions[] = {
    { "kdata_page",                0x00002000, REGION_SNAPSHOT_SIZE, FALSE, FALSE, {0}, {0} },
    { "bootctx_alias_A0006000",    0x00006000, REGION_SNAPSHOT_SIZE, FALSE, FALSE, {0}, {0} },
    { "bootparam_alias_A001D000",  0x0001D000, REGION_SNAPSHOT_SIZE, FALSE, FALSE, {0}, {0} },
    { "bootparam_alias_A002D000",  0x0002D000, REGION_SNAPSHOT_SIZE, FALSE, FALSE, {0}, {0} },
    { "callback_table_A0051680",   0x00051680, REGION_SNAPSHOT_SIZE, FALSE, FALSE, {0}, {0} },
    { "objptr_table_80660000",     0x00660000, REGION_SNAPSHOT_SIZE, FALSE, FALSE, {0}, {0} },
    { "obj_table_8066BFC0",        0x0066BFC0, REGION_SNAPSHOT_SIZE, FALSE, FALSE, {0}, {0} },
    { "obj_table_8067BFC0",        0x0067BFC0, REGION_SNAPSHOT_SIZE, FALSE, FALSE, {0}, {0} },
    { "callback_globals_80679400", 0x00679400, REGION_SNAPSHOT_SIZE, FALSE, FALSE, {0}, {0} },
    { "fptr_table_80075580",       0x00075400, REGION_SNAPSHOT_SIZE, FALSE, FALSE, {0}, {0} },
    { "vrc4173_base",              0x0A000000, REGION_SNAPSHOT_SIZE, FALSE, FALSE, {0}, {0} },
    { "vrc4173_page_A000",         0x0A00A000, REGION_SNAPSHOT_SIZE, FALSE, FALSE, {0}, {0} },
    { "vrc4173_page_C000",         0x0A00C000, REGION_SNAPSHOT_SIZE, FALSE, FALSE, {0}, {0} },
    { "vr4131_core_page",          0x0F000000, REGION_SNAPSHOT_SIZE, FALSE, FALSE, {0}, {0} },
};

static const focus_word_t g_focus_words[] = {
    { 0x0A000C38, "nand_sideband_c38",      NULL },
    { 0x0001D000, "bootparam_word0",        "0xA001D000" },
    { 0x0002D000, "bootparam2_word0",       "0xA002D000" },
    { 0x00051680, "callback_table_word0",   "0xA0051680" },
    { 0x00660000, "objptr_word0",           "0x80660000" },
    { 0x0066BFC0, "obj_table0_word0",       "0x8066BFC0" },
    { 0x0067BFC0, "obj_table1_word0",       "0x8067BFC0" },
    { 0x006794EC, "callback_g94EC",        "0x806794EC" },
    { 0x006794F0, "callback_g94F0",        "0x806794F0" },
    { 0x00679508, "callback_g9508",        "0x80679508" },
    { 0x00679510, "callback_g9510",        "0x80679510" },
    { 0x00075580, "fptr_table_slot0",      "0x80075580" },
    { 0x00075590, "fptr_table_slot4",      "0x80075590" },
};

static void Log(const char *fmt, ...)
{
    if (g_hFile == INVALID_HANDLE_VALUE)
        return;

    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    int n = _vsnprintf(buffer, sizeof(buffer) - 1, fmt, args);
    va_end(args);

    if (n < 0)
        buffer[sizeof(buffer) - 1] = '\0';
    else
        buffer[n] = '\0';

    DWORD written = 0;
    WriteFile(g_hFile, buffer, (DWORD)strlen(buffer), &written, NULL);
}

static void WideToAnsi(const WCHAR *src, char *dst, int dst_size)
{
    if (!dst || dst_size <= 0)
        return;
    dst[0] = '\0';
    if (!src)
        return;
    int ok = WideCharToMultiByte(CP_ACP, 0, src, -1, dst, dst_size, NULL, NULL);
    if (ok <= 0)
        _snprintf(dst, dst_size - 1, "<conv-failed>");
    dst[dst_size - 1] = '\0';
}

static void WideBytesToAnsi(const WCHAR *src, DWORD src_wchars_max, char *dst, int dst_size, BOOL *truncated)
{
    int out = 0;
    DWORD wlen = 0;
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

static DWORD ReadLE32(const BYTE *p)
{
    return ((DWORD)p[0]) |
           ((DWORD)p[1] << 8) |
           ((DWORD)p[2] << 16) |
           ((DWORD)p[3] << 24);
}

static void EnableKernelAccess(void)
{
    HMODULE hCore = GetModuleHandle(L"coredll.dll");
    if (!hCore) {
        Log("[PRIV] GetModuleHandle(coredll) failed\r\n");
        g_had_error = TRUE;
        return;
    }

    SETKMODE pSetKMode = (SETKMODE)GetProcAddress(hCore, L"SetKMode");
    SETPROCPERMISSIONS pSetPerms = (SETPROCPERMISSIONS)GetProcAddress(hCore, L"SetProcPermissions");

    if (pSetKMode) {
        BOOL km = pSetKMode(TRUE);
        Log("[PRIV] SetKMode(TRUE) -> %u\r\n", km ? 1u : 0u);
    } else {
        Log("[PRIV] SetKMode not available\r\n");
    }

    if (pSetPerms) {
        DWORD old = pSetPerms(0xFFFFFFFF);
        Log("[PRIV] SetProcPermissions(0xFFFFFFFF) old=0x%08X\r\n", old);
    } else {
        Log("[PRIV] SetProcPermissions not available\r\n");
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
        if (chunk > (0x1000u - page_off))
            chunk = 0x1000u - page_off;

        BYTE *vptr = (BYTE *)VirtualAlloc(0, 0x1000, MEM_RESERVE, PAGE_NOACCESS);
        if (!vptr) {
            Log("[MAP_FAIL] PA=0x%08X VirtualAlloc failed err=%lu\r\n", page_base, GetLastError());
            memset(out + done, 0, chunk);
            g_had_error = TRUE;
            ok = FALSE;
            done += chunk;
            continue;
        }

        if (!VirtualCopy(vptr, (LPVOID)(page_base >> 8), 0x1000, PAGE_READONLY | PAGE_NOCACHE | PAGE_PHYSICAL)) {
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

static void DumpRegionWords(const probe_region_t *r, const BYTE *data, BOOL ok, const char *phase_tag)
{
    Log("[REGION] phase=%s name=%s PA=0x%08X size=0x%04X status=%s\r\n",
        phase_tag, r->name, r->base, r->size, ok ? "OK" : "PARTIAL");

    DWORD off = 0;
    while (off + 16 <= r->size) {
        DWORD w0 = ReadLE32(data + off + 0);
        DWORD w1 = ReadLE32(data + off + 4);
        DWORD w2 = ReadLE32(data + off + 8);
        DWORD w3 = ReadLE32(data + off + 12);
        Log("0x%08X: %08X %08X %08X %08X\r\n",
            r->base + off, w0, w1, w2, w3);
        off += 16;
    }
}

static void CaptureSnapshot(BOOL post_phase)
{
    int i;
    for (i = 0; i < (int)(sizeof(g_regions) / sizeof(g_regions[0])); i++) {
        probe_region_t *r = &g_regions[i];
        BYTE *dst = post_phase ? r->post : r->baseline;
        BOOL ok = SafeReadPhysicalBytes(r->base, dst, r->size);
        if (post_phase)
            r->post_ok = ok;
        else
            r->baseline_ok = ok;
        __try {
            DumpRegionWords(r, dst, ok, post_phase ? "post" : "baseline");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[DUMP_FATAL] phase=%s name=%s PA=0x%08X code=0x%08lX\r\n",
                post_phase ? "post" : "baseline",
                r->name,
                r->base,
                GetExceptionCode());
            g_had_error = TRUE;
        }
    }
}

static BOOL FileExistsRegular(const WCHAR *path)
{
    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(path, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    BOOL ok = ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0);
    FindClose(h);
    return ok;
}

static BOOL SelectNandFile(WCHAR *out_path, DWORD out_cch)
{
    const WCHAR *preferred = L"\\Nand Disk\\hw_survey.exe";
    if (FileExistsRegular(preferred)) {
        CopyWide(out_path, out_cch, preferred);
        return TRUE;
    }

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(L"\\Nand Disk\\*", &fd);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;

    BOOL found = FALSE;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            WCHAR path_buf[MAX_PATH];
            wsprintf(path_buf, L"\\Nand Disk\\%s", fd.cFileName);
            CopyWide(out_path, out_cch, path_buf);
            found = TRUE;
            break;
        }
    } while (FindNextFile(h, &fd));

    FindClose(h);
    return found;
}

static BOOL RunNandWorkload(void)
{
    WCHAR file_path[MAX_PATH];
    if (!SelectNandFile(file_path, MAX_PATH)) {
        Log("[NAND] no regular file found under \\\\Nand Disk\r\n");
        g_had_error = TRUE;
        return FALSE;
    }

    char file_path_a[MAX_PATH];
    WideToAnsi(file_path, file_path_a, sizeof(file_path_a));
    Log("[NAND] selected_file=\"%s\" loops=%d\r\n", file_path_a, NAND_READ_LOOPS);

    BYTE buffer[4096];
    DWORD total_bytes = 0;
    int loop;
    for (loop = 0; loop < NAND_READ_LOOPS; loop++) {
        HANDLE h = CreateFile(file_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            Log("[NAND_LOOP] idx=%d open_failed err=%lu\r\n", loop + 1, GetLastError());
            g_had_error = TRUE;
            return FALSE;
        }

        DWORD loop_bytes = 0;
        BOOL loop_ok = TRUE;
        while (1) {
            DWORD got = 0;
            BOOL rd = ReadFile(h, buffer, sizeof(buffer), &got, NULL);
            if (!rd) {
                Log("[NAND_LOOP] idx=%d read_failed err=%lu\r\n", loop + 1, GetLastError());
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
        Log("[NAND_LOOP] idx=%d bytes=%lu status=%s\r\n",
            loop + 1, loop_bytes, loop_ok ? "OK" : "ERR");
        if (!loop_ok)
            return FALSE;
    }

    Log("[NAND] total_bytes=%lu loops=%d\r\n", total_bytes, NAND_READ_LOOPS);
    return TRUE;
}

static BOOL GetSnapshotWord(BOOL post_phase, DWORD pa, DWORD *out)
{
    int i;
    for (i = 0; i < (int)(sizeof(g_regions) / sizeof(g_regions[0])); i++) {
        probe_region_t *r = &g_regions[i];
        if (pa < r->base || pa + 4u > r->base + r->size)
            continue;

        const BYTE *src = post_phase ? r->post : r->baseline;
        BOOL ok = post_phase ? r->post_ok : r->baseline_ok;
        if (!ok)
            return FALSE;

        DWORD off = pa - r->base;
        *out = ReadLE32(src + off);
        return TRUE;
    }
    return FALSE;
}

static void EmitWordDiffs(void)
{
    DWORD total_changed = 0;
    int i;
    for (i = 0; i < (int)(sizeof(g_regions) / sizeof(g_regions[0])); i++) {
        probe_region_t *r = &g_regions[i];
        if (!r->baseline_ok || !r->post_ok) {
            Log("[DIFF_REGION] name=%s skipped baseline_ok=%u post_ok=%u\r\n",
                r->name, r->baseline_ok ? 1u : 0u, r->post_ok ? 1u : 0u);
            continue;
        }

        DWORD changed = 0;
        DWORD off;
        for (off = 0; off + 4u <= r->size; off += 4u) {
            DWORD before = ReadLE32(r->baseline + off);
            DWORD after = ReadLE32(r->post + off);
            if (before != after) {
                Log("[DIFF_WORD] name=%s PA=0x%08X before=0x%08X after=0x%08X\r\n",
                    r->name, r->base + off, before, after);
                changed++;
                total_changed++;
            }
        }
        Log("[DIFF_REGION] name=%s changed_words=%lu\r\n", r->name, changed);
    }
    Log("[DIFF_TOTAL] changed_words=%lu\r\n", total_changed);
}

static void EmitFocusSummary(void)
{
    int i;
    for (i = 0; i < (int)(sizeof(g_focus_words) / sizeof(g_focus_words[0])); i++) {
        const focus_word_t *f = &g_focus_words[i];
        DWORD before = 0;
        DWORD after = 0;
        BOOL ok_before = GetSnapshotWord(FALSE, f->pa, &before);
        BOOL ok_after = GetSnapshotWord(TRUE, f->pa, &after);

        if (!ok_before || !ok_after) {
            Log("[FOCUS] label=%s PA=0x%08X alias=%s status=UNREADABLE\r\n",
                f->label, f->pa, f->alias ? f->alias : "-");
            continue;
        }

        Log("[FOCUS] label=%s PA=0x%08X alias=%s before=0x%08X after=0x%08X status=%s\r\n",
            f->label, f->pa, f->alias ? f->alias : "-",
            before, after, (before == after) ? "UNCHANGED" : "CHANGED");
    }
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

    Log("TYPE=%lu SIZE=%lu HEX=", type, cb_data);
    DWORD i;
    for (i = 0; i < cb_data && i < 32u; i++)
        Log("%02X", (unsigned)data[i]);
    if (cb_data > 32u)
        Log("...");
}

static void DumpRegistryTree(const WCHAR *path, int depth)
{
    if (depth > MAX_REG_DUMP_DEPTH) {
        char path_a[512];
        WideToAnsi(path, path_a, sizeof(path_a));
        Log("[REG_KEY] path=\"HKLM\\\\%s\" status=DEPTH_LIMIT\r\n", path_a);
        return;
    }

    HKEY hKey = NULL;
    LONG open_rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &hKey);
    if (open_rc != ERROR_SUCCESS) {
        char path_a[512];
        WideToAnsi(path, path_a, sizeof(path_a));
        Log("[REG_KEY] path=\"HKLM\\\\%s\" open_failed=%ld\r\n", path_a, open_rc);
        return;
    }

    char path_a[512];
    WideToAnsi(path, path_a, sizeof(path_a));
    Log("[REG_KEY] path=\"HKLM\\\\%s\"\r\n", path_a);

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

        char value_name_a[256];
        if (value_name_len == 0)
            CopyAnsi(value_name_a, sizeof(value_name_a), "(Default)");
        else
            WideToAnsi(value_name, value_name_a, sizeof(value_name_a));

        Log("[REG_VALUE] name=\"%s\" ", value_name_a);
        LogValueData(type, data, data_len);
        Log("\r\n");

        vIndex++;
    }

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

        WCHAR next_path[512];
        wsprintf(next_path, L"%s\\%s", path, sub_name);
        DumpRegistryTree(next_path, depth + 1);

        kIndex++;
    }

    RegCloseKey(hKey);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nCmdShow)
{
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    g_hFile = CreateFile(L"\\BE300Probe_v1.txt", GENERIC_WRITE, 0, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_hFile == INVALID_HANDLE_VALUE) {
        MessageBox(NULL, L"Could not create \\BE300Probe_v1.txt", L"BE300 Probe v1", MB_OK | MB_ICONHAND);
        return 1;
    }

    __try {
        Log("BE-300 Post-Boot Probe Utility v1\r\n");
        Log("tick_ms=%lu\r\n", GetTickCount());

        EnableKernelAccess();

        Log("\r\n--- BASELINE SNAPSHOT ---\r\n");
        CaptureSnapshot(FALSE);

        Log("\r\n--- NAND WORKLOAD ---\r\n");
        if (!RunNandWorkload())
            g_had_error = TRUE;

        Log("\r\n--- POST SNAPSHOT ---\r\n");
        CaptureSnapshot(TRUE);

        Log("\r\n--- DIFF SUMMARY ---\r\n");
        EmitWordDiffs();
        EmitFocusSummary();

        Log("\r\n--- DRIVER INVENTORY ---\r\n");
        DumpRegistryTree(L"Drivers\\Active", 0);
        DumpRegistryTree(L"Drivers\\BuiltIn", 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[FATAL] code=0x%08lX\r\n", GetExceptionCode());
        g_had_error = TRUE;
    }

    Log("\r\nprobe_complete status=%s\r\n", g_had_error ? "PARTIAL_ERROR" : "OK");
    CloseHandle(g_hFile);
    g_hFile = INVALID_HANDLE_VALUE;

    if (g_had_error) {
        MessageBox(NULL,
                   L"BE300Probe v1 completed with partial errors.\nSee \\BE300Probe_v1.txt",
                   L"BE300 Probe v1",
                   MB_OK | MB_ICONEXCLAMATION);
        return 2;
    }

    MessageBox(NULL,
               L"BE300Probe v1 complete.\nSee \\BE300Probe_v1.txt",
               L"BE300 Probe v1",
               MB_OK | MB_ICONASTERISK);
    return 0;
}
