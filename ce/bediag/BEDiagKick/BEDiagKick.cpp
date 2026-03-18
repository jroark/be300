#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>

#define BEDIAG_KICK_BUILD_TAG  "basenameprobe1"
#define BEDIAG_MAX_TEXT        260

extern "C" HANDLE WINAPI ActivateDevice(LPCWSTR, DWORD);
extern "C" HANDLE WINAPI RegisterDevice(LPCWSTR, DWORD, LPCWSTR, DWORD);
extern "C" BOOL WINAPI DeregisterDevice(HANDLE);

typedef DWORD (WINAPI *PFN_BDG_INIT)(DWORD);
typedef BOOL (WINAPI *PFN_BDG_DEINIT)(DWORD);

static const WCHAR g_kick_log_path[] = L"\\Windows\\BEDiag_kick.txt";
static const WCHAR g_boot_log_path[] = L"\\Windows\\BEDiag_boot.txt";
static const WCHAR g_bediag_key[] = L"Drivers\\BuiltIn\\BEDiag";
static const WCHAR g_bediag_tmp_key[] = L"Drivers\\BuiltIn\\BEDiagTmp";
static const WCHAR g_active_root[] = L"Drivers\\Active";
static const WCHAR g_tmp_dll_path[] = L"\\Windows\\BEDiagTmp.dll";
static const WCHAR g_tmp_dll_name[] = L"BEDiagTmp.dll";

static HANDLE g_log = INVALID_HANDLE_VALUE;

static void WideToAnsi(const WCHAR *src, char *dst, int dst_size)
{
    int ok;

    if (!dst || dst_size <= 0)
        return;
    dst[0] = '\0';
    if (!src)
        return;

    ok = WideCharToMultiByte(CP_ACP, 0, src, -1, dst, dst_size, NULL, NULL);
    if (ok <= 0) {
        _snprintf(dst, dst_size - 1, "<conv-failed>");
        dst[dst_size - 1] = '\0';
    }
}

static void Logf(const char *fmt, ...)
{
    char buffer[1024];
    int n;
    va_list args;
    DWORD written;

    if (g_log == INVALID_HANDLE_VALUE)
        return;

    va_start(args, fmt);
    n = _vsnprintf(buffer, sizeof(buffer) - 1, fmt, args);
    va_end(args);

    if (n < 0)
        buffer[sizeof(buffer) - 1] = '\0';
    else
        buffer[n] = '\0';

    written = 0;
    WriteFile(g_log, buffer, (DWORD)strlen(buffer), &written, NULL);
    FlushFileBuffers(g_log);
}

static BOOL OpenKickLog(void)
{
    if (g_log != INVALID_HANDLE_VALUE)
        return TRUE;

    g_log = CreateFile(g_kick_log_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log == INVALID_HANDLE_VALUE)
        return FALSE;

    SetFilePointer(g_log, 0, NULL, FILE_END);
    return TRUE;
}

static BOOL FileExists(const WCHAR *path)
{
    DWORD attrs;

    attrs = GetFileAttributes(path);
    return attrs != 0xFFFFFFFFu;
}

static BOOL QueryStringValue(HKEY hKey, const WCHAR *name, WCHAR *out, DWORD out_cch)
{
    DWORD type;
    DWORD cb_data;
    LONG rc;

    if (!out || out_cch == 0)
        return FALSE;
    out[0] = L'\0';

    type = 0;
    cb_data = out_cch * sizeof(WCHAR);
    rc = RegQueryValueEx(hKey, name, NULL, &type, (LPBYTE)out, &cb_data);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        out[0] = L'\0';
        return FALSE;
    }

    out[out_cch - 1] = L'\0';
    return TRUE;
}

static BOOL QueryDWORDValue(HKEY hKey, const WCHAR *name, DWORD *out)
{
    DWORD type;
    DWORD cb_data;
    LONG rc;

    if (!out)
        return FALSE;
    *out = 0;

    type = 0;
    cb_data = sizeof(DWORD);
    rc = RegQueryValueEx(hKey, name, NULL, &type, (LPBYTE)out, &cb_data);
    return (rc == ERROR_SUCCESS && type == REG_DWORD && cb_data == sizeof(DWORD)) ? TRUE : FALSE;
}

static void LogPhaseMarker(const char *phase, const char *marker)
{
    Logf("[BEDIAG_KICK] phase=%s %s\r\n", phase, marker);
}

static void LogBreadcrumbState(const char *phase, const char *when)
{
    HKEY hKey;
    LONG rc;
    DWORD loaded;
    DWORD init_tick;
    DWORD worker_started;
    WCHAR last_status[128];
    char status_a[128];
    BOOL loaded_present;
    BOOL init_tick_present;
    BOOL worker_present;
    BOOL status_present;

    hKey = NULL;
    rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, g_bediag_key, 0, KEY_READ, &hKey);
    if (rc != ERROR_SUCCESS) {
        Logf("[BEDIAG_KICK] phase=%s breadcrumbs_%s key_present=no\r\n", phase, when);
        return;
    }

    loaded = 0;
    init_tick = 0;
    worker_started = 0;
    last_status[0] = L'\0';
    status_a[0] = '\0';

    loaded_present = QueryDWORDValue(hKey, L"BEDiagLoaded", &loaded);
    init_tick_present = QueryDWORDValue(hKey, L"BEDiagInitTick", &init_tick);
    worker_present = QueryDWORDValue(hKey, L"BEDiagWorkerStarted", &worker_started);
    status_present = QueryStringValue(hKey, L"BEDiagLastStatus", last_status, sizeof(last_status) / sizeof(last_status[0]));
    if (status_present)
        WideToAnsi(last_status, status_a, sizeof(status_a));

    Logf("[BEDIAG_KICK] phase=%s breadcrumbs_%s key_present=yes loaded_present=%s loaded=%lu init_tick_present=%s init_tick=%lu worker_present=%s worker=%lu status_present=%s status=\"%s\"\r\n",
         phase,
         when,
         loaded_present ? "yes" : "no",
         loaded,
         init_tick_present ? "yes" : "no",
         init_tick,
         worker_present ? "yes" : "no",
         worker_started,
         status_present ? "yes" : "no",
         status_a[0] ? status_a : "<missing>");

    RegCloseKey(hKey);
}

static void PrepareBootLogProbe(const char *phase)
{
    BOOL existed;
    BOOL deleted;
    DWORD gle;

    existed = FileExists(g_boot_log_path);
    deleted = FALSE;
    gle = 0;

    if (existed) {
        SetLastError(0);
        deleted = DeleteFile(g_boot_log_path);
        gle = GetLastError();
    }

    Logf("[BEDIAG_KICK] phase=%s boot_log_before=%u delete_attempted=%u delete_ok=%u delete_err=%lu\r\n",
         phase,
         existed ? 1u : 0u,
         existed ? 1u : 0u,
         deleted ? 1u : 0u,
         gle);
}

static BOOL WideEquals(const WCHAR *a, const WCHAR *b)
{
    DWORD i;

    if (!a || !b)
        return FALSE;
    for (i = 0;; i++) {
        WCHAR ca = a[i];
        WCHAR cb = b[i];

        if (ca >= L'A' && ca <= L'Z')
            ca = (WCHAR)(ca - L'A' + L'a');
        if (cb >= L'A' && cb <= L'Z')
            cb = (WCHAR)(cb - L'A' + L'a');
        if (ca != cb)
            return FALSE;
        if (ca == L'\0')
            return TRUE;
    }
}

static BOOL WideContainsInsensitive(const WCHAR *haystack, const WCHAR *needle)
{
    WCHAR hay[BEDIAG_MAX_TEXT];
    WCHAR ndl[BEDIAG_MAX_TEXT];
    DWORD i;

    if (!haystack || !needle)
        return FALSE;

    for (i = 0; i + 1 < BEDIAG_MAX_TEXT && haystack[i] != L'\0'; i++) {
        WCHAR ch = haystack[i];
        if (ch >= L'A' && ch <= L'Z')
            ch = (WCHAR)(ch - L'A' + L'a');
        hay[i] = ch;
    }
    hay[i] = L'\0';

    for (i = 0; i + 1 < BEDIAG_MAX_TEXT && needle[i] != L'\0'; i++) {
        WCHAR ch = needle[i];
        if (ch >= L'A' && ch <= L'Z')
            ch = (WCHAR)(ch - L'A' + L'a');
        ndl[i] = ch;
    }
    ndl[i] = L'\0';

    return wcsstr(hay, ndl) ? TRUE : FALSE;
}

static BOOL FindActiveMatch(const WCHAR *match_key,
                            const WCHAR *name_needle,
                            const WCHAR *dll_needle,
                            WCHAR *match_path,
                            DWORD match_path_cch)
{
    HKEY hRoot;
    LONG rc;
    DWORD kIndex;

    if (match_path && match_path_cch)
        match_path[0] = L'\0';

    hRoot = NULL;
    rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, g_active_root, 0, KEY_READ, &hRoot);
    if (rc != ERROR_SUCCESS)
        return FALSE;

    kIndex = 0;
    while (1) {
        WCHAR sub_name[128];
        DWORD sub_name_len;
        FILETIME ft;
        HKEY hSub;
        WCHAR full_path[256];
        WCHAR key_value[256];
        WCHAR name_value[128];
        WCHAR dll_value[128];
        BOOL is_match;

        sub_name_len = sizeof(sub_name) / sizeof(sub_name[0]);
        rc = RegEnumKeyEx(hRoot, kIndex, sub_name, &sub_name_len, NULL, NULL, NULL, &ft);
        if (rc == ERROR_NO_MORE_ITEMS)
            break;
        if (rc != ERROR_SUCCESS)
            break;

        sub_name[sub_name_len] = L'\0';
        wsprintf(full_path, L"%s\\%s", g_active_root, sub_name);
        hSub = NULL;
        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, full_path, 0, KEY_READ, &hSub) != ERROR_SUCCESS) {
            kIndex++;
            continue;
        }

        key_value[0] = L'\0';
        name_value[0] = L'\0';
        dll_value[0] = L'\0';
        QueryStringValue(hSub, L"Key", key_value, sizeof(key_value) / sizeof(key_value[0]));
        QueryStringValue(hSub, L"Name", name_value, sizeof(name_value) / sizeof(name_value[0]));
        QueryStringValue(hSub, L"Dll", dll_value, sizeof(dll_value) / sizeof(dll_value[0]));
        RegCloseKey(hSub);

        is_match = FALSE;
        if (match_key && match_key[0] != L'\0' && WideEquals(key_value, match_key))
            is_match = TRUE;
        if (name_needle && name_needle[0] != L'\0' && WideContainsInsensitive(name_value, name_needle))
            is_match = TRUE;
        if (dll_needle && dll_needle[0] != L'\0' && WideContainsInsensitive(dll_value, dll_needle))
            is_match = TRUE;

        if (is_match) {
            if (match_path && match_path_cch) {
                wcsncpy(match_path, full_path, match_path_cch - 1);
                match_path[match_path_cch - 1] = L'\0';
            }
            RegCloseKey(hRoot);
            return TRUE;
        }

        kIndex++;
    }

    RegCloseKey(hRoot);
    return FALSE;
}

static void LogActiveMatch(const char *phase,
                           const WCHAR *match_key,
                           const WCHAR *name_needle,
                           const WCHAR *dll_needle)
{
    WCHAR active_after[256];
    char active_after_a[256];

    active_after[0] = L'\0';
    if (FindActiveMatch(match_key, name_needle, dll_needle,
                        active_after, sizeof(active_after) / sizeof(active_after[0]))) {
        WideToAnsi(active_after, active_after_a, sizeof(active_after_a));
        Logf("[BEDIAG_KICK] phase=%s active_key_after=\"%s\"\r\n", phase, active_after_a);
    } else {
        Logf("[BEDIAG_KICK] phase=%s active_key_after=NONE\r\n", phase);
    }
}

static BOOL SetStringValue(HKEY hKey, const WCHAR *name, const WCHAR *value)
{
    DWORD cb_data;

    if (!value)
        value = L"";

    cb_data = ((DWORD)wcslen(value) + 1u) * sizeof(WCHAR);
    return RegSetValueEx(hKey, name, 0, REG_SZ, (const BYTE *)value, cb_data) == ERROR_SUCCESS ? TRUE : FALSE;
}

static BOOL SetDWORDValue(HKEY hKey, const WCHAR *name, DWORD value)
{
    return RegSetValueEx(hKey, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value)) == ERROR_SUCCESS ? TRUE : FALSE;
}

static BOOL CopyConfiguredDllToTemp(const WCHAR *src_path)
{
    BOOL copy_ok;
    DWORD gle;
    char src_a[256];
    char dst_a[256];

    src_a[0] = '\0';
    dst_a[0] = '\0';
    WideToAnsi(src_path, src_a, sizeof(src_a));
    WideToAnsi(g_tmp_dll_path, dst_a, sizeof(dst_a));

    DeleteFile(g_tmp_dll_path);
    SetLastError(0);
    copy_ok = CopyFile(src_path, g_tmp_dll_path, FALSE);
    gle = GetLastError();

    Logf("[BEDIAG_KICK] tmp_copy_src=\"%s\"\r\n", src_a[0] ? src_a : "<missing>");
    Logf("[BEDIAG_KICK] tmp_copy_dst=\"%s\"\r\n", dst_a);
    Logf("[BEDIAG_KICK] tmp_copy_ok=%s\r\n", copy_ok ? "yes" : "no");
    Logf("[BEDIAG_KICK] tmp_copy_err=%lu\r\n", gle);

    return copy_ok;
}

static BOOL CreateTempBuiltInKey(void)
{
    HKEY hKey;
    LONG rc;
    BOOL ok;

    RegDeleteKey(HKEY_LOCAL_MACHINE, g_bediag_tmp_key);

    hKey = NULL;
    rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, g_bediag_tmp_key, 0, NULL, 0, 0, NULL, &hKey, NULL);
    if (rc != ERROR_SUCCESS)
        return FALSE;

    ok = TRUE;
    ok = ok && SetStringValue(hKey, L"Dll", g_tmp_dll_name);
    ok = ok && SetStringValue(hKey, L"Prefix", L"BDT");
    ok = ok && SetDWORDValue(hKey, L"Index", 9);
    ok = ok && SetDWORDValue(hKey, L"Order", 1);

    RegCloseKey(hKey);
    return ok;
}

static void CleanupTempBuiltInKey(const char *phase)
{
    LONG rc;

    rc = RegDeleteKey(HKEY_LOCAL_MACHINE, g_bediag_tmp_key);
    Logf("[BEDIAG_KICK] phase=%s cleanup_temp_key_rc=%ld\r\n", phase, (long)rc);
}

static BOOL RunActivatePhase(const char *phase,
                             const WCHAR *activate_key,
                             const WCHAR *match_key,
                             const WCHAR *name_needle,
                             const WCHAR *dll_needle)
{
    HANDLE hDevice;
    DWORD gle;
    BOOL boot_after;

    LogPhaseMarker(phase, "begin");
    PrepareBootLogProbe(phase);
    LogBreadcrumbState(phase, "before");
    SetLastError(0);
    hDevice = ActivateDevice(activate_key, 0);
    gle = GetLastError();
    Logf("[BEDIAG_KICK] phase=%s activate_handle=0x%08lX last_error=%lu\r\n",
         phase,
         (DWORD)hDevice,
         gle);
    Sleep(500);
    LogActiveMatch(phase, match_key, name_needle, dll_needle);
    boot_after = FileExists(g_boot_log_path);
    Logf("[BEDIAG_KICK] phase=%s boot_log_after=%u\r\n", phase, boot_after ? 1u : 0u);
    LogBreadcrumbState(phase, "after");
    LogPhaseMarker(phase, "end");
    return hDevice ? TRUE : FALSE;
}

static BOOL RunRegisterDevicePhase(const char *phase,
                                   const WCHAR *prefix,
                                   DWORD index,
                                   const WCHAR *dll_path,
                                   const WCHAR *match_key,
                                   const WCHAR *name_needle,
                                   const WCHAR *dll_needle)
{
    HANDLE hRegDevice;
    HANDLE hOpenDevice;
    WCHAR device_name[32];
    DWORD gle;
    BOOL boot_after;
    BOOL dereg_ret;

    LogPhaseMarker(phase, "begin");
    PrepareBootLogProbe(phase);
    LogBreadcrumbState(phase, "before");

    hRegDevice = NULL;
    hOpenDevice = INVALID_HANDLE_VALUE;
    gle = 0;

    if (dll_path && dll_path[0] != L'\0') {
        SetLastError(0);
        hRegDevice = RegisterDevice(prefix, index, dll_path, 0);
        gle = GetLastError();
    }

    Logf("[BEDIAG_KICK] phase=%s register_handle=0x%08lX last_error=%lu\r\n",
         phase,
         (DWORD)hRegDevice,
         gle);

    if (hRegDevice) {
        Sleep(500);
        wsprintf(device_name, L"%s%lu:", prefix, index);
        SetLastError(0);
        hOpenDevice = CreateFile(device_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        gle = GetLastError();
        Logf("[BEDIAG_KICK] phase=%s createfile_handle=0x%08lX last_error=%lu\r\n",
             phase,
             (DWORD)hOpenDevice,
             gle);
        if (hOpenDevice != INVALID_HANDLE_VALUE) {
            CloseHandle(hOpenDevice);
            hOpenDevice = INVALID_HANDLE_VALUE;
        }
    } else {
        Logf("[BEDIAG_KICK] phase=%s createfile_handle=SKIPPED\r\n", phase);
    }

    Sleep(500);
    LogActiveMatch(phase, match_key, name_needle, dll_needle);
    boot_after = FileExists(g_boot_log_path);
    Logf("[BEDIAG_KICK] phase=%s boot_log_after=%u\r\n", phase, boot_after ? 1u : 0u);
    LogBreadcrumbState(phase, "after");

    if (hRegDevice) {
        SetLastError(0);
        dereg_ret = DeregisterDevice(hRegDevice);
        gle = GetLastError();
        Logf("[BEDIAG_KICK] phase=%s deregister_ret=%s last_error=%lu\r\n",
             phase,
             dereg_ret ? "yes" : "no",
             gle);
    } else {
        Logf("[BEDIAG_KICK] phase=%s deregister_ret=SKIPPED\r\n", phase);
    }

    LogPhaseMarker(phase, "end");
    return hRegDevice ? TRUE : FALSE;
}

static DWORD RunDirectInitPhase(const char *phase,
                                PFN_BDG_INIT pfn_bdg_init,
                                PFN_BDG_DEINIT pfn_bdg_deinit,
                                DWORD context,
                                const WCHAR *context_path)
{
    DWORD bdg_init_ret;
    DWORD gle;
    BOOL bdg_deinit_ret;
    BOOL boot_after;
    char context_a[256];

    context_a[0] = '\0';
    if (context_path)
        WideToAnsi(context_path, context_a, sizeof(context_a));

    LogPhaseMarker(phase, "begin");
    PrepareBootLogProbe(phase);
    LogBreadcrumbState(phase, "before");
    Logf("[BEDIAG_KICK] phase=%s bdg_init_context=0x%08lX context_path=\"%s\"\r\n",
         phase,
         context,
         context_a[0] ? context_a : "<none>");

    bdg_init_ret = 0;
    bdg_deinit_ret = FALSE;
    if (pfn_bdg_init)
        bdg_init_ret = pfn_bdg_init(context);

    Logf("[BEDIAG_KICK] phase=%s bdg_init_ret=0x%08lX\r\n", phase, bdg_init_ret);
    Sleep(500);

    if (bdg_init_ret != 0 && pfn_bdg_deinit) {
        SetLastError(0);
        bdg_deinit_ret = pfn_bdg_deinit(bdg_init_ret);
        gle = GetLastError();
        Logf("[BEDIAG_KICK] phase=%s bdg_deinit_ret=%s last_error=%lu\r\n",
             phase,
             bdg_deinit_ret ? "yes" : "no",
             gle);
    } else {
        Logf("[BEDIAG_KICK] phase=%s bdg_deinit_ret=SKIPPED\r\n", phase);
    }

    boot_after = FileExists(g_boot_log_path);
    Logf("[BEDIAG_KICK] phase=%s boot_log_after=%u\r\n", phase, boot_after ? 1u : 0u);
    LogBreadcrumbState(phase, "after");
    LogPhaseMarker(phase, "end");
    return bdg_init_ret;
}

static FARPROC LogExportState(HMODULE hModule, const WCHAR *export_name)
{
    FARPROC proc;
    char export_a[64];

    proc = GetProcAddress(hModule, export_name);
    WideToAnsi(export_name, export_a, sizeof(export_a));
    Logf("[BEDIAG_KICK] export_%s=%s proc=0x%08lX\r\n",
         export_a,
         proc ? "yes" : "no",
         (DWORD)proc);
    return proc;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmdLine, int nShowCmd)
{
    HKEY hKey;
    LONG rc;
    BOOL boot_before;
    BOOL dll_exists;
    HMODULE hBediag;
    DWORD gle;
    WCHAR active_after[256];
    WCHAR dll_path[256];
    WCHAR prefix_value[64];
    char active_after_a[256];
    char dll_a[256];
    char prefix_a[64];
    DWORD index_value;
    DWORD order_value;
    BOOL reg_dll_present;
    BOOL reg_prefix_present;
    BOOL reg_index_present;
    BOOL reg_order_present;
    PFN_BDG_INIT pfn_bdg_init;
    PFN_BDG_DEINIT pfn_bdg_deinit;
    BOOL tmp_copy_ok;
    BOOL temp_key_ok;
    BOOL any_success;
    DWORD bdg_init_null_ret;
    DWORD bdg_init_builtin_ret;

    (void)hInst;
    (void)hPrev;
    (void)lpCmdLine;
    (void)nShowCmd;

    if (!OpenKickLog())
        return 1;

    boot_before = FileExists(g_boot_log_path);
    Logf("[BEDIAG_KICK] build=%s tick_ms=%lu key=\"Drivers\\BuiltIn\\BEDiag\" boot_log_before=%u\r\n",
         BEDIAG_KICK_BUILD_TAG,
         GetTickCount(),
         boot_before ? 1u : 0u);

    dll_path[0] = L'\0';
    prefix_value[0] = L'\0';
    dll_a[0] = '\0';
    prefix_a[0] = '\0';
    index_value = 0;
    order_value = 0;
    reg_dll_present = FALSE;
    reg_prefix_present = FALSE;
    reg_index_present = FALSE;
    reg_order_present = FALSE;

    hKey = NULL;
    rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, g_bediag_key, 0, KEY_READ, &hKey);
    Logf("[BEDIAG_KICK] reg_key_exists=%s\r\n", rc == ERROR_SUCCESS ? "yes" : "no");
    if (rc == ERROR_SUCCESS) {
        reg_dll_present = QueryStringValue(hKey, L"Dll", dll_path, sizeof(dll_path) / sizeof(dll_path[0]));
        reg_prefix_present = QueryStringValue(hKey, L"Prefix", prefix_value, sizeof(prefix_value) / sizeof(prefix_value[0]));
        reg_index_present = QueryDWORDValue(hKey, L"Index", &index_value);
        reg_order_present = QueryDWORDValue(hKey, L"Order", &order_value);

        Logf("[BEDIAG_KICK] reg_dll_present=%s\r\n", reg_dll_present ? "yes" : "no");
        WideToAnsi(dll_path, dll_a, sizeof(dll_a));
        Logf("[BEDIAG_KICK] reg_dll=\"%s\"\r\n", dll_a[0] ? dll_a : "<missing>");

        Logf("[BEDIAG_KICK] reg_prefix_present=%s\r\n", reg_prefix_present ? "yes" : "no");
        WideToAnsi(prefix_value, prefix_a, sizeof(prefix_a));
        Logf("[BEDIAG_KICK] reg_prefix=\"%s\"\r\n", prefix_a[0] ? prefix_a : "<missing>");

        Logf("[BEDIAG_KICK] reg_index_present=%s\r\n", reg_index_present ? "yes" : "no");
        Logf("[BEDIAG_KICK] reg_index=%lu\r\n", index_value);

        Logf("[BEDIAG_KICK] reg_order_present=%s\r\n", reg_order_present ? "yes" : "no");
        Logf("[BEDIAG_KICK] reg_order=%lu\r\n", order_value);

        RegCloseKey(hKey);
    }

    dll_exists = (reg_dll_present && dll_path[0] != L'\0') ? FileExists(dll_path) : FALSE;
    Logf("[BEDIAG_KICK] dll_exists=%s path=\"%s\"\r\n",
         dll_exists ? "yes" : "no",
         dll_a[0] ? dll_a : "<missing>");

    active_after[0] = L'\0';
    if (FindActiveMatch(g_bediag_key, NULL, L"BEDiag.dll",
                        active_after, sizeof(active_after) / sizeof(active_after[0]))) {
        WideToAnsi(active_after, active_after_a, sizeof(active_after_a));
        Logf("[BEDIAG_KICK] active_key_before=\"%s\"\r\n", active_after_a);
    } else {
        Logf("[BEDIAG_KICK] active_key_before=NONE\r\n");
    }

    pfn_bdg_init = NULL;
    pfn_bdg_deinit = NULL;
    hBediag = NULL;
    gle = 0;
    if (reg_dll_present && dll_path[0] != L'\0') {
        SetLastError(0);
        hBediag = LoadLibrary(dll_path);
        gle = GetLastError();
    }
    Logf("[BEDIAG_KICK] loadlibrary=%s handle=0x%08lX last_error=%lu\r\n",
         hBediag ? "yes" : "no",
         (DWORD)hBediag,
         gle);
    Logf("[BEDIAG_KICK] loadlibrary_path=\"%s\"\r\n",
         dll_a[0] ? dll_a : "<missing>");
    if (hBediag) {
        pfn_bdg_init = (PFN_BDG_INIT)LogExportState(hBediag, L"BDG_Init");
        pfn_bdg_deinit = (PFN_BDG_DEINIT)LogExportState(hBediag, L"BDG_Deinit");
        LogExportState(hBediag, L"BDG_Open");
        LogExportState(hBediag, L"BDG_Close");
        LogExportState(hBediag, L"BDG_IOControl");
    } else {
        Logf("[BEDIAG_KICK] export_BDG_Init=no proc=0x00000000\r\n");
        Logf("[BEDIAG_KICK] export_BDG_Deinit=no proc=0x00000000\r\n");
        Logf("[BEDIAG_KICK] export_BDG_Open=no proc=0x00000000\r\n");
        Logf("[BEDIAG_KICK] export_BDG_Close=no proc=0x00000000\r\n");
        Logf("[BEDIAG_KICK] export_BDG_IOControl=no proc=0x00000000\r\n");
    }

    any_success = FALSE;

    if (RunActivatePhase("activate_builtin",
                         g_bediag_key,
                         g_bediag_key,
                         NULL,
                         L"BEDiag.dll"))
        any_success = TRUE;

    if (RunRegisterDevicePhase("register_device",
                               L"BDG",
                               9,
                               reg_dll_present ? dll_path : NULL,
                               g_bediag_key,
                               NULL,
                               L"BEDiag.dll"))
        any_success = TRUE;

    tmp_copy_ok = FALSE;
    if (reg_dll_present && dll_path[0] != L'\0')
        tmp_copy_ok = CopyConfiguredDllToTemp(dll_path);
    else
        Logf("[BEDIAG_KICK] tmp_copy_src=\"<missing>\"\r\n[BEDIAG_KICK] tmp_copy_dst=\"\\Windows\\BEDiagTmp.dll\"\r\n[BEDIAG_KICK] tmp_copy_ok=no\r\n[BEDIAG_KICK] tmp_copy_err=0\r\n");

    if (tmp_copy_ok) {
        if (RunRegisterDevicePhase("register_device_windows_copy",
                                   L"BDG",
                                   9,
                                   g_tmp_dll_name,
                                   g_bediag_tmp_key,
                                   NULL,
                                   L"BEDiagTmp.dll"))
            any_success = TRUE;
    } else {
        LogPhaseMarker("register_device_windows_copy", "begin");
        Logf("[BEDIAG_KICK] phase=register_device_windows_copy skipped_reason=tmp_copy_failed\r\n");
        LogPhaseMarker("register_device_windows_copy", "end");
    }

    temp_key_ok = FALSE;
    if (tmp_copy_ok)
        temp_key_ok = CreateTempBuiltInKey();
    Logf("[BEDIAG_KICK] temp_key_create_ok=%s\r\n", temp_key_ok ? "yes" : "no");

    if (temp_key_ok) {
        if (RunActivatePhase("activate_builtin_windows_copy",
                             g_bediag_tmp_key,
                             g_bediag_tmp_key,
                             NULL,
                             L"BEDiagTmp.dll"))
            any_success = TRUE;
        CleanupTempBuiltInKey("activate_builtin_windows_copy");
    } else {
        LogPhaseMarker("activate_builtin_windows_copy", "begin");
        Logf("[BEDIAG_KICK] phase=activate_builtin_windows_copy skipped_reason=temp_key_create_failed\r\n");
        LogPhaseMarker("activate_builtin_windows_copy", "end");
    }

    bdg_init_null_ret = RunDirectInitPhase("direct_bdg_init_nullctx",
                                           pfn_bdg_init,
                                           pfn_bdg_deinit,
                                           0,
                                           NULL);
    if (bdg_init_null_ret != 0)
        any_success = TRUE;

    bdg_init_builtin_ret = RunDirectInitPhase("direct_bdg_init_builtinctx",
                                              pfn_bdg_init,
                                              pfn_bdg_deinit,
                                              (DWORD)g_bediag_key,
                                              g_bediag_key);
    if (bdg_init_builtin_ret != 0)
        any_success = TRUE;

    if (hBediag)
        FreeLibrary(hBediag);

    CloseHandle(g_log);
    g_log = INVALID_HANDLE_VALUE;
    return any_success ? 0 : 1;
}
