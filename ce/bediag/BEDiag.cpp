#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

extern "C" BOOL VirtualCopy(LPVOID, LPVOID, DWORD, DWORD);

#ifndef PAGE_PHYSICAL
#define PAGE_PHYSICAL 0x40000000
#endif

#ifndef PAGE_NOCACHE
#define PAGE_NOCACHE 0x0200
#endif

#define BEDIAG_MAX_REGION_SIZE   0x1000
#define BEDIAG_MAX_PATH_LEN      260
#define BEDIAG_BACKLOG_SIZE      0x80000
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
    BOOL ok[PHASE_COUNT];
    DWORD crc[PHASE_COUNT];
    BYTE data[PHASE_COUNT][BEDIAG_MAX_REGION_SIZE];
} bediag_region_t;

typedef struct {
    DWORD pa;
    const char *label;
} focus_word_t;

typedef struct {
    DWORD init_tick;
    DWORD init_context;
    HANDLE worker_thread;
    DWORD worker_thread_id;
    volatile LONG stop_requested;
    WCHAR active_key[256];
    BOOL worker_started;
} bediag_driver_t;

static const char *g_phase_names[PHASE_COUNT] = {
    "init",
    "plus1s",
    "plus5s"
};

static bediag_region_t g_regions[] = {
    { "resume_ctx",    0x00002200u, 0x0100u, { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "bootctx",       0x00006000u, 0x1000u, { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "bootparam0",    0x0001D000u, 0x1000u, { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "bootparam1",    0x0002D000u, 0x1000u, { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "cb_tbl",        0x00051680u, 0x0480u, { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "objptr",        0x00660000u, 0x0040u, { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "obj_header",    0x0066BFC0u, 0x0020u, { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "postboot_text", 0x00679400u, 0x0200u, { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "vrc4173_cwin",  0x0A000C00u, 0x0050u, { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} },
    { "vr4131_safe",   0x0F000000u, 0x0120u, { FALSE, FALSE, FALSE }, { 0, 0, 0 }, {{0}, {0}, {0}} }
};

static const focus_word_t g_focus_words[] = {
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
    { 0x00660000u, "objptr_0000" },
    { 0x0066BFC0u, "objhdr_bfc0" },
    { 0x0066BFC4u, "objhdr_bfc4" },
    { 0x0066BFC8u, "objhdr_bfc8" },
    { 0x0066BFCCu, "objhdr_bfcc" }
};

static bediag_driver_t g_driver;
static HANDLE g_serial = INVALID_HANDLE_VALUE;
static HANDLE g_file = INVALID_HANDLE_VALUE;
static DWORD g_serial_open_error = 0;
static BOOL g_serial_retry_done = FALSE;
static BOOL g_serial_failure_reported = FALSE;
static BOOL g_had_error = FALSE;
static BOOL g_backlog_overflow = FALSE;
static char g_backlog[BEDIAG_BACKLOG_SIZE];
static DWORD g_backlog_used = 0;
static WCHAR g_file_path[BEDIAG_MAX_PATH_LEN];

static const WCHAR *g_file_roots[] = {
    L"\\Nand Disk",
    L"\\Storage Card",
    L"\\Temp",
    L"\\"
};

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

    tick = g_driver.init_tick ? g_driver.init_tick : GetTickCount();
    for (i = 0; i < (int)(sizeof(g_file_roots) / sizeof(g_file_roots[0])); i++) {
        int suffix;
        const WCHAR *root;
        root = g_file_roots[i];
        if (!(root[0] == L'\\' && root[1] == L'\0') && !DirectoryExists(root))
            continue;

        for (suffix = 0; suffix < 16; suffix++) {
            WCHAR candidate[BEDIAG_MAX_PATH_LEN];
            DWORD written;

            BuildFileCandidate(root, tick, suffix, candidate, BEDIAG_MAX_PATH_LEN);
            g_file = CreateFile(candidate, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
            if (g_file == INVALID_HANDLE_VALUE)
                continue;

            CopyWide(g_file_path, BEDIAG_MAX_PATH_LEN, candidate);
            written = 0;
            if (g_backlog_used != 0)
                WriteFile(g_file, g_backlog, g_backlog_used, &written, NULL);
            if (g_backlog_overflow) {
                const char *overflow = "[BEDIAG] backlog_overflow=1\r\n";
                WriteFile(g_file, overflow, (DWORD)strlen(overflow), &written, NULL);
            }
            FlushFileBuffers(g_file);
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

static void BeginSection(const char *title)
{
    TryOpenSecondaryFile();
    MaybeReportSerialFailure();
    Logf("\r\n--- %s ---\r\n", title);
    FlushSinks();
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

static void CapturePhase(snapshot_phase_t phase)
{
    int i;
    for (i = 0; i < (int)(sizeof(g_regions) / sizeof(g_regions[0])); i++) {
        bediag_region_t *region;
        const char *changed;

        region = &g_regions[i];
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

static DWORD WINAPI BEDiagWorkerThread(LPVOID arg)
{
    bediag_driver_t *driver;
    const char *final_status;

    driver = (bediag_driver_t *)arg;
    driver->worker_started = TRUE;
    driver->worker_thread_id = GetCurrentThreadId();

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
    EmitFocusWords(PHASE_INIT);
    CaptureRegistryState(PHASE_INIT);

    if (!driver->stop_requested)
        Sleep(1000);

    MaybeRetrySerial();
    TryOpenSecondaryFile();
    MaybeReportSerialFailure();

    if (!driver->stop_requested) {
        BeginSection("SNAPSHOT +1S");
        Logf("tick_ms=%lu phase=%s\r\n", GetTickCount(), g_phase_names[PHASE_PLUS1S]);
        CapturePhase(PHASE_PLUS1S);
        EmitFocusWords(PHASE_PLUS1S);
        CaptureRegistryState(PHASE_PLUS1S);
    }

    if (!driver->stop_requested)
        Sleep(4000);

    TryOpenSecondaryFile();
    MaybeReportSerialFailure();

    if (!driver->stop_requested) {
        BeginSection("SNAPSHOT +5S");
        Logf("tick_ms=%lu phase=%s\r\n", GetTickCount(), g_phase_names[PHASE_PLUS5S]);
        CapturePhase(PHASE_PLUS5S);
        EmitFocusWords(PHASE_PLUS5S);
        CaptureRegistryState(PHASE_PLUS5S);
    }

    BeginSection("DRIVER STATE");
    LogDriverState();

    BeginSection("BEDIAG DONE");
    Logf("tick_ms=%lu\r\n", GetTickCount());
    final_status = driver->stop_requested ? "ABORTED" : (g_had_error ? "PARTIAL_ERROR" : "OK");
    Logf("status=%s\r\n", final_status);
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
    g_driver.init_context = dwContext;
    CopyContextPath(dwContext, g_driver.active_key, sizeof(g_driver.active_key) / sizeof(g_driver.active_key[0]));

    OpenSerialLog();

    g_driver.worker_thread = CreateThread(NULL, 0, BEDiagWorkerThread, &g_driver, 0, &g_driver.worker_thread_id);
    if (!g_driver.worker_thread) {
        TryOpenSecondaryFile();
        MaybeReportSerialFailure();
        BeginSection("BEDIAG INIT");
        Logf("tick_ms=%lu\r\n", GetTickCount());
        Logf("worker_start_failed err=%lu\r\n", GetLastError());
        BeginSection("BEDIAG DONE");
        Logf("status=FAILED\r\n");
        FlushSinks();
        g_had_error = TRUE;
        return 0;
    }

    return (DWORD)&g_driver;
}

extern "C" BOOL WINAPI BDG_Deinit(DWORD hDeviceContext)
{
    bediag_driver_t *driver;

    driver = (bediag_driver_t *)hDeviceContext;
    if (driver != &g_driver)
        return FALSE;

    InterlockedExchange(&driver->stop_requested, 1);
    if (driver->worker_thread) {
        WaitForSingleObject(driver->worker_thread, 2000);
        CloseHandle(driver->worker_thread);
        driver->worker_thread = NULL;
    }

    CloseLogs();
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
