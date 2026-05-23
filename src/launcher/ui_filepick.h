#ifndef BE300_LAUNCHER_UI_FILEPICK_H
#define BE300_LAUNCHER_UI_FILEPICK_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Thin wrapper around nativefiledialog-extended (NFDe). The rest of the
 * launcher does not include NFD headers. Each function returns true on a
 * confirmed selection and fills out_path. False means the user cancelled
 * or NFDe errored. */

/* Call once at process start, paired with ui_filepick_shutdown. */
bool ui_filepick_init(void);
void ui_filepick_shutdown(void);

/* Open-file dialog. If `extensions_csv` is non-null (e.g., "bin,img"), the
 * picker is filtered to those extensions. */
bool ui_filepick_open(const char *title,
                      const char *extensions_csv,
                      const char *default_path,
                      char *out_path, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* BE300_LAUNCHER_UI_FILEPICK_H */
