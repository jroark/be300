#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define BEDIAG_KICK_BUILD_TAG  "loadproof1"

static const WCHAR g_kick_log_path[] = L"\\Windows\\BEDiag_kick.txt";
static const WCHAR g_boot_log_path[] = L"\\Windows\\BEDiag_boot.txt";
static const WCHAR g_bediag_key[] = L"Drivers\\BuiltIn\\BEDiag";

static HANDLE g_log = INVALID_HANDLE_VALUE;

typedef HANDLE (WINAPI *PFN_ACTIVATEDEVICE)(LPCWSTR, DWORD);
typedef HANDLE (WINAPI *PFN_ACTIVATEDEVICEEX)(LPCWSTR, const BYTE *, DWORD, LPVOID);

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

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmdLine, int nShowCmd)
{
    HMODULE coredll;
    PFN_ACTIVATEDEVICE activate_device;
    PFN_ACTIVATEDEVICEEX activate_device_ex;
    HANDLE hDevice;
    DWORD gle;
    BOOL boot_before;
    BOOL boot_after;
    const char *api_name;

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

    coredll = GetModuleHandle(L"coredll.dll");
    if (!coredll)
        coredll = LoadLibrary(L"coredll.dll");

    if (!coredll) {
        Logf("[BEDIAG_KICK] status=NO_COREDLL err=%lu\r\n", GetLastError());
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
        return 1;
    }

    activate_device_ex = (PFN_ACTIVATEDEVICEEX)GetProcAddress(coredll, "ActivateDeviceEx");
    activate_device = (PFN_ACTIVATEDEVICE)GetProcAddress(coredll, "ActivateDevice");

    hDevice = NULL;
    api_name = NULL;
    SetLastError(0);

    if (activate_device_ex) {
        api_name = "ActivateDeviceEx";
        hDevice = activate_device_ex(g_bediag_key, NULL, 0, NULL);
    } else if (activate_device) {
        api_name = "ActivateDevice";
        hDevice = activate_device(g_bediag_key, 0);
    } else {
        Logf("[BEDIAG_KICK] status=NO_ACTIVATE_API\r\n");
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
        return 1;
    }

    gle = GetLastError();
    Logf("[BEDIAG_KICK] api=%s handle=0x%08lX last_error=%lu\r\n",
         api_name,
         (DWORD)hDevice,
         gle);

    Sleep(500);
    boot_after = FileExists(g_boot_log_path);
    Logf("[BEDIAG_KICK] boot_log_after=%u\r\n", boot_after ? 1u : 0u);

    CloseHandle(g_log);
    g_log = INVALID_HANDLE_VALUE;
    return hDevice ? 0 : 1;
}
