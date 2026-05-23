#ifndef BE300_LAUNCHER_STATE_H
#define BE300_LAUNCHER_STATE_H

#include <SDL.h>
#include <stdbool.h>
#include "vm_bundle.h"
#include "launcher_screenshot.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LAUNCHER_VIEW_MANAGER = 0,
    LAUNCHER_VIEW_WIZARD,
    LAUNCHER_VIEW_SETTINGS,
} launcher_view_t;

typedef struct launcher_state {
    SDL_Window         *win;
    SDL_Renderer       *ren;
    vm_bundle_list_t    vms;
    int                 selected_vm;  /* -1 = none */
    launcher_view_t     view;
    bool                quit;
    bool                want_run;     /* manager -> request to launch selected VM */
    screenshot_cache_t  shot;         /* cached most-recent screenshot */
} launcher_state_t;

/* Manager (left rail + details). Returns true if a draw happened. */
void launcher_manager_draw(launcher_state_t *L);

/* New-VM wizard view. */
void launcher_wizard_draw(launcher_state_t *L);

/* Edit-existing-VM settings view. */
void launcher_settings_draw(launcher_state_t *L);
void launcher_settings_reset(void);

/* Re-scan the VM root dir. Frees and rebuilds L->vms. */
void launcher_refresh_vms(launcher_state_t *L);

#ifdef __cplusplus
}
#endif

#endif /* BE300_LAUNCHER_STATE_H */
