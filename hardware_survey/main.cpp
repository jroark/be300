#include <windows.h>
#include <stdio.h>
#include <string.h>

extern "C" BOOL VirtualCopy(LPVOID, LPVOID, DWORD, DWORD);
typedef BOOL (*SETKMODE)(BOOL);
typedef DWORD (*SETPROCPERMISSIONS)(DWORD);

HANDLE g_hFile = INVALID_HANDLE_VALUE;

void Log(const char* fmt, ...) {
    if (g_hFile == INVALID_HANDLE_VALUE) return;
    char buffer[512];
    va_list args; va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);
    DWORD written;
    WriteFile(g_hFile, buffer, strlen(buffer), &written, NULL);
}

void SafeDumpPage(const char* label, DWORD physAddr) {
    Log("\r\n--- %s (PA: 0x%08X) ---\r\n", label, physAddr);
    void* vPtr = VirtualAlloc(0, 0x1000, MEM_RESERVE, PAGE_NOACCESS);
    if (!vPtr) return;
    if (VirtualCopy(vPtr, (void*)(physAddr >> 8), 0x1000, PAGE_READONLY | PAGE_NOCACHE | PAGE_PHYSICAL)) {
        volatile DWORD* pData = (volatile DWORD*)vPtr;
        for (int i = 0; i < 128; i++) {
            if (i % 4 == 0) Log("0x%03X: ", i * 4);
            __try { Log("%08X ", pData[i]); } __except(EXCEPTION_EXECUTE_HANDLER) { Log("XXXXXXXX "); }
            if ((i + 1) % 4 == 0) Log("\r\n");
        }
    } else {
        Log("VirtualCopy failed (Error %d)\r\n", GetLastError());
    }
    VirtualFree(vPtr, 0, MEM_RELEASE);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nCmdShow) {
    HMODULE hCore = GetModuleHandle(L"coredll.dll");
    SETKMODE pSetKMode = (SETKMODE)GetProcAddress(hCore, L"SetKMode");
    if (pSetKMode) pSetKMode(TRUE);
    SETPROCPERMISSIONS pSetPerms = (SETPROCPERMISSIONS)GetProcAddress(hCore, L"SetProcPermissions");
    if (pSetPerms) pSetPerms(0xFFFFFFFF);

    g_hFile = CreateFile(L"\\HardwareDump6.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_hFile == INVALID_HANDLE_VALUE) return 1;

    Log("BE-300 Hardware Survey v6 (PCI Focus)\r\n");

    // 1. VR4131 Internal (Check CMU and PCI Unit control)
    SafeDumpPage("VR4131 Core Page", 0x0F000000);
    SafeDumpPage("VR4131 PCI Unit", 0x0F000C00); // PCI windows are here!

    // 2. The new "hot" range from Linux setup.c
    SafeDumpPage("PCI I/O Base (setup.c)", 0x0A00C000);

    // 3. Scan the 0x0A00xxxx range more broadly in 4KB steps
    Log("\r\n--- Broad Scan of 0x0A00xxxx ---\r\n");
    for (DWORD pa = 0x0A000000; pa <= 0x0A010000; pa += 0x1000) {
        void* vPtr = VirtualAlloc(0, 0x1000, MEM_RESERVE, PAGE_NOACCESS);
        if (VirtualCopy(vPtr, (void*)(pa >> 8), 0x1000, PAGE_READONLY | PAGE_NOCACHE | PAGE_PHYSICAL)) {
            __try {
                DWORD val = *(volatile DWORD*)vPtr;
                if (val != 0 && val != 0xFFFFFFFF) {
                    Log("DETECTED non-zero at PA 0x%08X: 0x%08X\r\n", pa, val);
                    SafeDumpPage("Detailed Dump", pa);
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
        }
        VirtualFree(vPtr, 0, MEM_RELEASE);
    }

    CloseHandle(g_hFile);
    MessageBox(NULL, L"Survey v6 Complete!", L"Success", MB_OK);
    return 0;
}
