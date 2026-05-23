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
    /* "explore" verb opens the path in Explorer; on a directory it opens
     * the folder, on a file it selects+opens its parent. */
    ShellExecuteA(NULL, "explore", path, NULL, NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    /* `open <dir>` opens the Finder window for that directory. Bundle
     * paths come from sanitize_name() which strips quote/shell-meta
     * characters, so the system() invocation is safe. */
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "open \"%s\" >/dev/null 2>&1 &", path);
    (void)system(cmd);
#else
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "xdg-open \"%s\" >/dev/null 2>&1 &", path);
    (void)system(cmd);
#endif
}
