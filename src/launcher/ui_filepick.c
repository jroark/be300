#include "ui_filepick.h"

#include <nfd.h>
#include <stdio.h>
#include <string.h>

bool ui_filepick_init(void)
{
    return NFD_Init() == NFD_OKAY;
}

void ui_filepick_shutdown(void)
{
    NFD_Quit();
}

bool ui_filepick_open(const char *title,
                      const char *extensions_csv,
                      const char *default_path,
                      char *out_path, size_t out_cap)
{
    (void)title; /* NFDe doesn't expose a per-call title on every platform. */

    if (!out_path || out_cap == 0) return false;
    out_path[0] = '\0';

    nfdu8filteritem_t filter = { "Files", extensions_csv ? extensions_csv : "*" };
    nfdu8char_t *picked = NULL;
    nfdresult_t r = NFD_OpenDialogU8(&picked,
        extensions_csv ? &filter : NULL,
        extensions_csv ? 1u : 0u,
        default_path);
    if (r != NFD_OKAY || !picked) return false;

    size_t n = strlen(picked);
    if (n + 1 > out_cap) n = out_cap - 1;
    memcpy(out_path, picked, n);
    out_path[n] = '\0';
    NFD_FreePathU8(picked);
    return true;
}
