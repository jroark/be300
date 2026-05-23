#include "launcher_screenshot.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dirent.h>
#endif

static int newest_bmp_in(const char *dir, char *out_path, size_t out_cap,
                         time_t *out_mtime)
{
    if (!dir || !*dir || !out_path || out_cap == 0) return -1;
    out_path[0] = '\0';
    time_t best_mtime = 0;
    bool   have_best  = false;

#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s\\*.bmp", dir);
    WIN32_FIND_DATAA find;
    HANDLE h = FindFirstFileA(pattern, &find);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s\\%s", dir, find.cFileName);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (!have_best || st.st_mtime > best_mtime) {
            have_best = true;
            best_mtime = st.st_mtime;
            snprintf(out_path, out_cap, "%s", path);
        }
    } while (FindNextFileA(h, &find));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        size_t n = strlen(name);
        if (n < 5) continue;
        if (strcasecmp(name + n - 4, ".bmp") != 0) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (!have_best || st.st_mtime > best_mtime) {
            have_best = true;
            best_mtime = st.st_mtime;
            snprintf(out_path, out_cap, "%s", path);
        }
    }
    closedir(d);
#endif

    if (!have_best) return -1;
    if (out_mtime) *out_mtime = best_mtime;
    return 0;
}

void launcher_screenshot_release(screenshot_cache_t *cache)
{
    if (!cache) return;
    if (cache->tex) {
        SDL_DestroyTexture(cache->tex);
        cache->tex = NULL;
    }
    cache->tex_w = cache->tex_h = 0;
    cache->loaded_from[0] = '\0';
    cache->loaded_mtime = 0;
}

bool launcher_screenshot_refresh(screenshot_cache_t *cache,
                                 SDL_Renderer       *ren,
                                 const char         *bundle_path)
{
    if (!cache || !ren || !bundle_path || !*bundle_path) return false;

    /* The emulator's ui_save_screenshot() writes screenshot_YYYYMMDD_*.bmp
     * to its working directory. launcher_main.c::run_vm_in_process() chdirs
     * into the bundle root before the run, so the BMPs land at
     * <bundle>/screenshot_*.bmp — not <bundle>/screenshots/*.bmp. Look in
     * both places and pick the newest match (legacy bundles that pre-date
     * the chdir change may have screenshots elsewhere). */
    char path[1024], alt_path[1024];
    time_t mtime = 0, alt_mtime = 0;
    int root_ok = newest_bmp_in(bundle_path, path, sizeof path, &mtime);

    char sub[1024];
#ifdef _WIN32
    snprintf(sub, sizeof sub, "%s\\screenshots", bundle_path);
#else
    snprintf(sub, sizeof sub, "%s/screenshots", bundle_path);
#endif
    int sub_ok = newest_bmp_in(sub, alt_path, sizeof alt_path, &alt_mtime);

    if (sub_ok == 0 && (root_ok != 0 || alt_mtime > mtime)) {
        snprintf(path, sizeof path, "%s", alt_path);
        mtime = alt_mtime;
        root_ok = 0;
    }
    if (root_ok != 0) {
        launcher_screenshot_release(cache);
        return false;
    }
    if (cache->tex &&
        strcmp(cache->loaded_from, path) == 0 &&
        cache->loaded_mtime == mtime) {
        return true; /* still current */
    }

    SDL_Surface *surf = SDL_LoadBMP(path);
    if (!surf) {
        launcher_screenshot_release(cache);
        return false;
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
    int w = surf->w, h = surf->h;
    SDL_FreeSurface(surf);
    if (!tex) {
        launcher_screenshot_release(cache);
        return false;
    }
    launcher_screenshot_release(cache);
    cache->tex = tex;
    cache->tex_w = w;
    cache->tex_h = h;
    snprintf(cache->loaded_from, sizeof cache->loaded_from, "%s", path);
    cache->loaded_mtime = mtime;
    return true;
}
