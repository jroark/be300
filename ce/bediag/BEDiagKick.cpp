#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>

#define BEDIAG_KICK_BUILD_TAG  "activationproof1"
#define BEDIAG_MAX_TEXT        260

extern "C" HANDLE WINAPI ActivateDevice(LPCWSTR, DWORD);

static const WCHAR g_kick_log_path[] = L"\\Windows\\BEDiag_kick.txt";
static const WCHAR g_boot_log_path[] = L"\\Windows\\BEDiag_boot.txt";
static const WCHAR g_bediag_key[] = L"Drivers\\BuiltIn\\BEDiag";
static const WCHAR g_active_root[] = L"Drivers\\Active";

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

static BOOL FindActiveBEDiag(WCHAR *match_path, DWORD match_path_cch)
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
        if (WideEquals(key_value, g_bediag_key))
            is_match = TRUE;
        if (WideContainsInsensitive(name_value, L"BEDiag"))
            is_match = TRUE;
        if (WideContainsInsensitive(dll_value, L"BEDiag"))
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

static void LogExportState(HMODULE hModule, const WCHAR *export_name)
{
    FARPROC proc;
    char export_a[64];

    proc = GetProcAddress(hModule, export_name);
    WideToAnsi(export_name, export_a, sizeof(export_a));
    Logf("[BEDIAG_KICK] export_%s=%s proc=0x%08lX\r\n",
         export_a,
         proc ? "yes" : "no",
         (DWORD)proc);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmdLine, int nShowCmd)
{
    HKEY hKey;
    LONG rc;
    BOOL boot_before;
    BOOL boot_after;
    BOOL dll_exists;
    HMODULE hBediag;
    DWORD gle;
    HANDLE hDevice;
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
    if (FindActiveBEDiag(active_after, sizeof(active_after) / sizeof(active_after[0]))) {
        WideToAnsi(active_after, active_after_a, sizeof(active_after_a));
        Logf("[BEDIAG_KICK] active_key_before=\"%s\"\r\n", active_after_a);
    } else {
        Logf("[BEDIAG_KICK] active_key_before=NONE\r\n");
    }

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
        LogExportState(hBediag, L"BDG_Init");
        LogExportState(hBediag, L"BDG_Deinit");
        LogExportState(hBediag, L"BDG_Open");
        LogExportState(hBediag, L"BDG_Close");
        LogExportState(hBediag, L"BDG_IOControl");
        FreeLibrary(hBediag);
    } else {
        Logf("[BEDIAG_KICK] export_BDG_Init=no proc=0x00000000\r\n");
        Logf("[BEDIAG_KICK] export_BDG_Deinit=no proc=0x00000000\r\n");
        Logf("[BEDIAG_KICK] export_BDG_Open=no proc=0x00000000\r\n");
        Logf("[BEDIAG_KICK] export_BDG_Close=no proc=0x00000000\r\n");
        Logf("[BEDIAG_KICK] export_BDG_IOControl=no proc=0x00000000\r\n");
    }

    SetLastError(0);
    hDevice = ActivateDevice(g_bediag_key, 0);
    gle = GetLastError();
    Logf("[BEDIAG_KICK] activate_handle=0x%08lX last_error=%lu\r\n",
         (DWORD)hDevice,
         gle);

    Sleep(500);
    active_after[0] = L'\0';
    if (FindActiveBEDiag(active_after, sizeof(active_after) / sizeof(active_after[0]))) {
        WideToAnsi(active_after, active_after_a, sizeof(active_after_a));
        Logf("[BEDIAG_KICK] active_key_after=\"%s\"\r\n", active_after_a);
    } else {
        Logf("[BEDIAG_KICK] active_key_after=NONE\r\n");
    }

    boot_after = FileExists(g_boot_log_path);
    Logf("[BEDIAG_KICK] boot_log_after=%u\r\n", boot_after ? 1u : 0u);

    CloseHandle(g_log);
    g_log = INVALID_HANDLE_VALUE;
    return hDevice ? 0 : 1;
}
