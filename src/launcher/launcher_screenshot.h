#ifndef BE300_LAUNCHER_SCREENSHOT_H
#define BE300_LAUNCHER_SCREENSHOT_H

#include <SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cached most-recent screenshot for the currently-selected VM. The cache
 * is invalidated when the bundle path or the file's mtime changes. */
typedef struct screenshot_cache {
    SDL_Texture *tex;
    int          tex_w;
    int          tex_h;
    char         loaded_from[1024];
    time_t       loaded_mtime;
} screenshot_cache_t;

/* Locate the newest *.bmp under <bundle_path>/screenshots/ and, if it
 * differs from what is currently cached, load it into a new SDL_Texture.
 * Returns true if the cache holds a valid texture afterwards. */
bool launcher_screenshot_refresh(screenshot_cache_t *cache,
                                 SDL_Renderer       *ren,
                                 const char         *bundle_path);

/* Drop the cached texture (e.g., on shutdown or when no VM is selected). */
void launcher_screenshot_release(screenshot_cache_t *cache);

#ifdef __cplusplus
}
#endif

#endif /* BE300_LAUNCHER_SCREENSHOT_H */
