#include "launcher_os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shellapi.h>
#endif

void launcher_reveal_in_file_manager(const char *path)
{
    if (!path || !*path) return;

#ifdef _WIN32
    /* Spawn explorer.exe directly. ShellExecuteA with "explore"/"open"
     * routes through COM-based shell extensions that require
     * OleInitialize on the calling thread and silently no-op when it's
     * missing; CreateProcessA sidesteps all of that. */
    char cmdline[2048];
    snprintf(cmdline, sizeof cmdline, "explorer.exe \"%s\"", path);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL,
                       &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return;
    }
    /* Fallback: try ShellExecuteA in case explorer.exe isn't on PATH
     * (unusual but possible inside a stripped Windows container). */
    HINSTANCE rc = ShellExecuteA(NULL, "open", path, NULL, NULL,
                                 SW_SHOWNORMAL);
    if ((INT_PTR)rc <= 32) {
        fprintf(stderr, "[launcher] reveal failed for \"%s\" "
                "(CreateProcess err=%lu, ShellExecute rc=%lld)\n",
                path, (unsigned long)GetLastError(), (long long)(INT_PTR)rc);
    }
#elif defined(__APPLE__)
    /* `open <dir>` opens the Finder window for that directory. Bundle
     * paths come from sanitize_name() which strips quote/shell-meta
     * characters, so the system() invocation is safe. */
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "open \"%s\" >/dev/null 2>&1 &", path);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "[launcher] reveal failed for \"%s\" "
                "(open exit %d)\n", path, rc);
    }
#else
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "xdg-open \"%s\" >/dev/null 2>&1 &", path);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "[launcher] reveal failed for \"%s\" "
                "(xdg-open exit %d)\n", path, rc);
    }
#endif
}
