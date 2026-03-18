#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>

#define BDGMINI_LOG_PATH L"\\Windows\\BDGMini_boot.txt"
#define BDGMINI_KEY_PATH L"Drivers\\BuiltIn\\BDGMini"

typedef struct bdgmini_driver_t {
    DWORD init_tick;
    DWORD init_context;
    BOOL active;
} bdgmini_driver_t;

static bdgmini_driver_t g_driver;

static void AppendLogf(const char *fmt, ...)
{
    HANDLE hFile;
    char buffer[512];
    int n;
    va_list args;
    DWORD written;

    hFile = CreateFile(BDGMINI_LOG_PATH,
                       GENERIC_WRITE,
                       FILE_SHARE_READ,
                       NULL,
                       OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL,
                       NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return;

    SetFilePointer(hFile, 0, NULL, FILE_END);

    va_start(args, fmt);
    n = _vsnprintf(buffer, sizeof(buffer) - 1, fmt, args);
    va_end(args);

    if (n < 0)
        buffer[sizeof(buffer) - 1] = '\0';
    else
        buffer[n] = '\0';

    written = 0;
    WriteFile(hFile, buffer, (DWORD)strlen(buffer), &written, NULL);
    FlushFileBuffers(hFile);
    CloseHandle(hFile);
}

static void SetBreadcrumbDWORD(const WCHAR *name, DWORD value)
{
    HKEY hKey;
    LONG rc;

    hKey = NULL;
    rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, BDGMINI_KEY_PATH, 0, NULL, 0, 0, NULL, &hKey, NULL);
    if (rc != ERROR_SUCCESS)
        return;

    RegSetValueEx(hKey, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
    RegCloseKey(hKey);
}

static void SetBreadcrumbString(const WCHAR *name, const WCHAR *value)
{
    HKEY hKey;
    LONG rc;
    DWORD cb_data;

    hKey = NULL;
    rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, BDGMINI_KEY_PATH, 0, NULL, 0, 0, NULL, &hKey, NULL);
    if (rc != ERROR_SUCCESS)
        return;

    if (!value)
        value = L"";
    cb_data = ((DWORD)wcslen(value) + 1u) * sizeof(WCHAR);
    RegSetValueEx(hKey, name, 0, REG_SZ, (const BYTE *)value, cb_data);
    RegCloseKey(hKey);
}

extern "C" BOOL WINAPI DllMain(HANDLE hInst, DWORD reason, LPVOID reserved)
{
    (void)hInst;
    (void)reason;
    (void)reserved;
    return TRUE;
}

extern "C" DWORD WINAPI MDG_Init(DWORD dwContext)
{
    memset(&g_driver, 0, sizeof(g_driver));
    g_driver.init_tick = GetTickCount();
    g_driver.init_context = dwContext;
    g_driver.active = TRUE;

    SetBreadcrumbDWORD(L"BDGMiniLoaded", 1);
    SetBreadcrumbDWORD(L"BDGMiniInitTick", g_driver.init_tick);
    SetBreadcrumbDWORD(L"BDGMiniWorkerStarted", 0);
    SetBreadcrumbString(L"BDGMiniLastStatus", L"INIT");

    AppendLogf("[BDGMINI_STAGE] stage=init_enter tick_ms=%lu context=0x%08lX\r\n",
               g_driver.init_tick,
               dwContext);
    return (DWORD)&g_driver;
}

extern "C" BOOL WINAPI MDG_Deinit(DWORD hDeviceContext)
{
    if ((bdgmini_driver_t *)hDeviceContext != &g_driver)
        return FALSE;

    AppendLogf("[BDGMINI_STAGE] stage=deinit tick_ms=%lu context=0x%08lX\r\n",
               GetTickCount(),
               g_driver.init_context);
    SetBreadcrumbString(L"BDGMiniLastStatus", L"DEINIT");
    g_driver.active = FALSE;
    return TRUE;
}

extern "C" DWORD WINAPI MDG_Open(DWORD hDeviceContext, DWORD AccessCode, DWORD ShareMode)
{
    (void)AccessCode;
    (void)ShareMode;
    return hDeviceContext;
}

extern "C" BOOL WINAPI MDG_Close(DWORD hOpenContext)
{
    (void)hOpenContext;
    return TRUE;
}

extern "C" DWORD WINAPI MDG_Read(DWORD hOpenContext, LPVOID pBuffer, DWORD Count)
{
    (void)hOpenContext;
    (void)pBuffer;
    (void)Count;
    SetLastError(ERROR_NOT_SUPPORTED);
    return (DWORD)-1;
}

extern "C" DWORD WINAPI MDG_Write(DWORD hOpenContext, LPCVOID pBuffer, DWORD Count)
{
    (void)hOpenContext;
    (void)pBuffer;
    (void)Count;
    SetLastError(ERROR_NOT_SUPPORTED);
    return (DWORD)-1;
}

extern "C" DWORD WINAPI MDG_Seek(DWORD hOpenContext, long Amount, WORD Type)
{
    (void)hOpenContext;
    (void)Amount;
    (void)Type;
    SetLastError(ERROR_NOT_SUPPORTED);
    return (DWORD)-1;
}

extern "C" BOOL WINAPI MDG_IOControl(DWORD hOpenContext,
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

extern "C" void WINAPI MDG_PowerDown(void)
{
    AppendLogf("[BDGMINI_STAGE] stage=powerdown tick_ms=%lu\r\n", GetTickCount());
}

extern "C" void WINAPI MDG_PowerUp(void)
{
    AppendLogf("[BDGMINI_STAGE] stage=powerup tick_ms=%lu\r\n", GetTickCount());
}
