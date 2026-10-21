/*
 *  src/ui_macos.c — the frame window's transparency and click-through
 *  mask on macOS, where SDL's shaped windows alone leave the corners
 *  opaque; no-ops elsewhere. Split out of src/ui.c.
 */

#include "ui.h"
#include "ui_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_SDL2

#include <SDL.h>
#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <SDL_syswm.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#endif

#ifdef __APPLE__
static void ui_macos_release_cg_data(void *info, const void *data,
    size_t size)
{
    (void)info;
    (void)size;
    free((void *)data);
}

void ui_macos_make_window_transparent(SDL_Window *win)
{
    SDL_SysWMinfo info;
    Class ns_color_class;
    id ns_window;
    id content_view;
    id layer;
    id clear_color;
    id clear_cg_color = NULL;

    if (!win)
        return;

    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) ||
        info.subsystem != SDL_SYSWM_COCOA)
        return;

    ns_window = (id)info.info.cocoa.window;
    if (!ns_window)
        return;

    ((void (*)(id, SEL, BOOL))objc_msgSend)(ns_window,
        sel_registerName("setOpaque:"), (BOOL)0);

    ns_color_class = (Class)objc_getClass("NSColor");
    if (ns_color_class) {
        clear_color = ((id (*)(Class, SEL))objc_msgSend)(ns_color_class,
            sel_registerName("clearColor"));
        if (clear_color) {
            ((void (*)(id, SEL, id))objc_msgSend)(ns_window,
                sel_registerName("setBackgroundColor:"), clear_color);
            clear_cg_color = ((id (*)(id, SEL))objc_msgSend)(clear_color,
                sel_registerName("CGColor"));
        }
    }

    content_view = ((id (*)(id, SEL))objc_msgSend)(ns_window,
        sel_registerName("contentView"));
    if (!content_view)
        return;

    ((void (*)(id, SEL, BOOL))objc_msgSend)(content_view,
        sel_registerName("setWantsLayer:"), (BOOL)1);
    layer = ((id (*)(id, SEL))objc_msgSend)(content_view,
        sel_registerName("layer"));
    if (!layer)
        return;

    ((void (*)(id, SEL, BOOL))objc_msgSend)(layer,
        sel_registerName("setOpaque:"), (BOOL)0);
    if (clear_cg_color) {
        ((void (*)(id, SEL, id))objc_msgSend)(layer,
            sel_registerName("setBackgroundColor:"), clear_cg_color);
    }
}

void ui_macos_apply_window_mask(SDL_Window *win,
    const uint8_t *window_mask, int win_w, int win_h)
{
    SDL_SysWMinfo info;
    Class ca_layer_class;
    id ns_window;
    id content_view;
    id layer;
    id mask_layer;
    CGColorSpaceRef color_space = NULL;
    CGDataProviderRef provider = NULL;
    CGImageRef image = NULL;
    uint8_t *rgba = NULL;
    size_t pixels;
    CGRect bounds;

    if (!win || !window_mask || win_w <= 0 || win_h <= 0)
        return;

    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) ||
        info.subsystem != SDL_SYSWM_COCOA)
        return;

    ns_window = (id)info.info.cocoa.window;
    if (!ns_window)
        return;

    content_view = ((id (*)(id, SEL))objc_msgSend)(ns_window,
        sel_registerName("contentView"));
    if (!content_view)
        return;

    ((void (*)(id, SEL, BOOL))objc_msgSend)(content_view,
        sel_registerName("setWantsLayer:"), (BOOL)1);
    layer = ((id (*)(id, SEL))objc_msgSend)(content_view,
        sel_registerName("layer"));
    if (!layer)
        return;

    pixels = (size_t)win_w * (size_t)win_h;
    rgba = malloc(pixels * 4u);
    if (!rgba)
        return;
    for (size_t i = 0; i < pixels; i++) {
        rgba[i * 4u + 0u] = 255;
        rgba[i * 4u + 1u] = 255;
        rgba[i * 4u + 2u] = 255;
        rgba[i * 4u + 3u] = window_mask[i];
    }

    color_space = CGColorSpaceCreateDeviceRGB();
    if (!color_space)
        goto out;
    provider = CGDataProviderCreateWithData(NULL, rgba, pixels * 4u,
        ui_macos_release_cg_data);
    if (!provider)
        goto out;
    rgba = NULL;  /* owned by provider */

    image = CGImageCreate((size_t)win_w, (size_t)win_h, 8, 32,
        (size_t)win_w * 4u, color_space,
        kCGBitmapByteOrder32Big | kCGImageAlphaLast,
        provider, NULL, false, kCGRenderingIntentDefault);
    if (!image)
        goto out;

    ca_layer_class = (Class)objc_getClass("CALayer");
    if (!ca_layer_class)
        goto out;
    mask_layer = ((id (*)(Class, SEL))objc_msgSend)(ca_layer_class,
        sel_registerName("layer"));
    if (!mask_layer)
        goto out;

    bounds = CGRectMake(0.0, 0.0, (CGFloat)win_w, (CGFloat)win_h);
    ((void (*)(id, SEL, CGRect))objc_msgSend)(mask_layer,
        sel_registerName("setFrame:"), bounds);
    ((void (*)(id, SEL, id))objc_msgSend)(mask_layer,
        sel_registerName("setContents:"), (id)image);
    ((void (*)(id, SEL, BOOL))objc_msgSend)(layer,
        sel_registerName("setMasksToBounds:"), (BOOL)1);
    ((void (*)(id, SEL, id))objc_msgSend)(layer,
        sel_registerName("setMask:"), mask_layer);

out:
    free(rgba);
    if (image)
        CGImageRelease(image);
    if (provider)
        CGDataProviderRelease(provider);
    if (color_space)
        CGColorSpaceRelease(color_space);
}
#else
void ui_macos_make_window_transparent(SDL_Window *win)
{
    (void)win;
}

void ui_macos_apply_window_mask(SDL_Window *win,
    const uint8_t *window_mask, int win_w, int win_h)
{
    (void)win;
    (void)window_mask;
    (void)win_w;
    (void)win_h;
}
#endif

#endif  /* HAVE_SDL2 */
