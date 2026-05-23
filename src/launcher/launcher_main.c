/* Launcher entry — Phase 3: in-process Run transition.
 *
 * When the user clicks Run on the manager view we:
 *   1. shut down ImGui and destroy the launcher's SDL window/renderer
 *   2. chdir into the VM bundle (so screenshots and nk_decompressed.bin
 *      dumps land inside the bundle, not the user's cwd)
 *   3. call be300_create/run/destroy with the bundle's machine_config_t
 *   4. chdir back, rebuild the launcher window/renderer, re-init ImGui
 *
 * SDL_INIT_VIDEO stays initialized across the transition because src/ui.c
 * was patched to only QuitSubSystem when it transitioned the subsystem
 * from off → on (see ui.c:1859 and ui.c:266). */

#include "launcher.h"
#include "launcher_state.h"
#include "ui_imgui_backend.h"
#include "ui_filepick.h"
#include "vm_bundle.h"
#include "be300.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define be300_chdir _chdir
#define be300_getcwd _getcwd
#else
#include <unistd.h>
#define be300_chdir chdir
#define be300_getcwd getcwd
#endif

#define LAUNCHER_WIN_W 960
#define LAUNCHER_WIN_H 600

static const char *LAUNCHER_TITLE = "BE-300 VM Manager";

static SDL_Renderer *make_renderer(SDL_Window *win)
{
    SDL_Renderer *r = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!r) r = SDL_CreateRenderer(win, -1, 0);
    return r;
}

static SDL_Window *make_window(void)
{
    return SDL_CreateWindow(LAUNCHER_TITLE,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        LAUNCHER_WIN_W, LAUNCHER_WIN_H,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
}

/* Tear down launcher SDL state, run the emulator in-process, then rebuild
 * launcher SDL state. Returns 0 on success even if the emulator errored —
 * the launcher should always come back. */
static int run_vm_in_process(launcher_state_t *L, const vm_bundle_t *src_vm)
{
    /* Snapshot the bundle: L->vms may be re-allocated on refresh after the
     * run, which would invalidate cfg's pointers. */
    vm_bundle_t vm;
    memcpy(&vm, src_vm, sizeof vm);
    vm_bundle_rebind_strings(&vm);

    /* 1. Shut down launcher ImGui + SDL window. Keep SDL_INIT_VIDEO up. */
    launcher_imgui_shutdown();
    if (L->ren) { SDL_DestroyRenderer(L->ren); L->ren = NULL; }
    if (L->win) { SDL_DestroyWindow(L->win); L->win = NULL; }

    /* 2. chdir into the bundle so per-run artifacts (screenshots, NK dumps)
     *    land inside it. */
    char saved_cwd[BE300_VM_PATH_MAX];
    if (!be300_getcwd(saved_cwd, sizeof saved_cwd)) saved_cwd[0] = '\0';
    if (vm.path[0] && be300_chdir(vm.path) != 0) {
        fprintf(stderr, "[launcher] chdir to %s failed; running from cwd.\n",
            vm.path);
    }

    /* 3. Run the emulator. */
    fprintf(stderr, "[launcher] Booting \"%s\" — NAND=%s\n",
        vm.name,
        vm.cfg.nand_path ? vm.cfg.nand_path : "(none)");
    machine_t *m = be300_create(&vm.cfg);
    if (m) {
        be300_run(m);
        be300_destroy(m);
    } else {
        fprintf(stderr, "[launcher] be300_create failed — returning to manager.\n");
    }

    /* 4. Restore cwd and rebuild launcher window. */
    if (saved_cwd[0]) (void)be300_chdir(saved_cwd);

    L->win = make_window();
    if (!L->win) {
        fprintf(stderr, "[launcher] Could not recreate launcher window: %s\n",
            SDL_GetError());
        L->quit = true;
        return -1;
    }
    L->ren = make_renderer(L->win);
    if (!L->ren) {
        fprintf(stderr, "[launcher] Could not recreate launcher renderer: %s\n",
            SDL_GetError());
        SDL_DestroyWindow(L->win);
        L->win = NULL;
        L->quit = true;
        return -1;
    }
    if (!launcher_imgui_init(L->win, L->ren)) {
        fprintf(stderr, "[launcher] Could not reinitialise ImGui.\n");
        SDL_DestroyRenderer(L->ren);
        SDL_DestroyWindow(L->win);
        L->ren = NULL; L->win = NULL;
        L->quit = true;
        return -1;
    }

    /* Re-scan VMs (screenshots/, mtimes etc. may have changed). */
    launcher_refresh_vms(L);
    return 0;
}

static int launcher_run_loop(launcher_state_t *L)
{
    while (!L->quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            launcher_imgui_process_event(&ev);
            if (ev.type == SDL_QUIT) L->quit = true;
            if (ev.type == SDL_WINDOWEVENT &&
                ev.window.event == SDL_WINDOWEVENT_CLOSE &&
                L->win && ev.window.windowID == SDL_GetWindowID(L->win))
                L->quit = true;
        }
        if (L->quit) break;

        launcher_imgui_new_frame();

        switch (L->view) {
        case LAUNCHER_VIEW_MANAGER:
        default:
            launcher_manager_draw(L);
            break;
        case LAUNCHER_VIEW_WIZARD:
            launcher_wizard_draw(L);
            break;
        case LAUNCHER_VIEW_SETTINGS:
            launcher_settings_draw(L);
            break;
        }

        launcher_imgui_apply_hidpi_scale(L->win, L->ren);
        SDL_SetRenderDrawColor(L->ren, 32, 32, 36, 255);
        SDL_RenderClear(L->ren);
        launcher_imgui_render(L->ren);
        SDL_RenderPresent(L->ren);

        if (L->want_run) {
            L->want_run = false;
            if (L->selected_vm < 0 ||
                L->selected_vm >= (int)L->vms.count) continue;
            const vm_bundle_t *vm = &L->vms.items[L->selected_vm];
            if (!vm->cfg.nand_path && !vm->cfg.rom_path && !vm->cfg.restore) {
                fprintf(stderr,
                    "[launcher] VM \"%s\" has no boot media (set --nand via "
                    "Edit/Settings first).\n", vm->name);
                continue;
            }
            run_vm_in_process(L, vm);
        }

        SDL_Delay(16); /* ~60 fps cap */
    }
    return 0;
}

int launcher_main(int argc, char *argv[])
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "launcher: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *win = make_window();
    if (!win) {
        fprintf(stderr, "launcher: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_Renderer *ren = make_renderer(win);
    if (!ren) {
        fprintf(stderr, "launcher: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    if (!launcher_imgui_init(win, ren)) {
        fprintf(stderr, "launcher: ImGui init failed\n");
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    if (!ui_filepick_init()) {
        fprintf(stderr, "launcher: native file dialog init failed; "
            "file picker may not work\n");
    }

    launcher_state_t L = {0};
    L.win = win;
    L.ren = ren;
    L.selected_vm = -1;
    L.view = LAUNCHER_VIEW_MANAGER;
    launcher_refresh_vms(&L);

    /* If invoked as `be300 <path-to-bundle>.be300vm`, auto-run that VM
     * once at startup (Finder double-click path on macOS / file-association
     * path on Windows). */
    if (argc == 2 && argv && argv[1]) {
        vm_bundle_t loaded;
        if (vm_bundle_open_from_path(argv[1], &loaded) == 0) {
            run_vm_in_process(&L, &loaded);
        }
    }

    int rc = launcher_run_loop(&L);

    vm_bundle_list_free(&L.vms);
    if (L.win || L.ren) {
        launcher_imgui_shutdown();
        if (L.ren) SDL_DestroyRenderer(L.ren);
        if (L.win) SDL_DestroyWindow(L.win);
    }
    ui_filepick_shutdown();
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    SDL_Quit();
    return rc;
}
