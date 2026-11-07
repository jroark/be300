/*
 *  ui_platform_macos.m — Cocoa integration for the SDL frontend: the
 *  application menu bar, the frameless window's unified toolbar, the bezel
 *  window's transparency mask and shadow, the clipboard and NSOpenPanel.
 *  See ui_platform.h. Built with ARC.
 */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include "ui_platform.h"
#include "ui_commands.h"

#include <SDL_syswm.h>
#include <stdlib.h>
#include <string.h>

#ifndef BE300_VERSION_STRING
#define BE300_VERSION_STRING "0.1.0"
#endif

static NSWindow *ns_window_for(SDL_Window *win)
{
    SDL_SysWMinfo info;

    if (!win)
        return nil;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_COCOA)
        return nil;
    return info.info.cocoa.window;
}

static bool cocoa_video(void)
{
    const char *drv = SDL_GetCurrentVideoDriver();
    return drv && strcmp(drv, "cocoa") == 0 && NSApp != nil;
}

static const ui_menu_item_t *item_for(ui_cmd_t cmd)
{
    for (size_t i = 0; i < ui_menu_item_count; i++)
        if (ui_menu_items[i].cmd == cmd)
            return &ui_menu_items[i];
    return NULL;
}

static NSString *const kToolbarRocket = @"be300.rocket";
static NSString *const kToolbarPower = @"be300.power";
static NSString *const kToolbarShot = @"be300.screenshot";

@interface BE300MenuTarget : NSObject <NSMenuItemValidation, NSToolbarDelegate>
@end

@implementation BE300MenuTarget

- (void)perform:(id)sender
{
    ui_cmd_post((ui_cmd_t)[sender tag]);
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    const ui_menu_item_t *it;

    if (item.action != @selector(perform:))
        return YES;
    it = item_for((ui_cmd_t)item.tag);
    if (it && (it->flags & UI_ITEM_CHECKABLE))
        item.state = ui_cmd_checked((ui_cmd_t)item.tag) ?
            NSControlStateValueOn : NSControlStateValueOff;
    return ui_cmd_enabled((ui_cmd_t)item.tag);
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarDefaultItemIdentifiers:(NSToolbar *)toolbar
{
    (void)toolbar;
    return @[ NSToolbarFlexibleSpaceItemIdentifier, kToolbarRocket,
              kToolbarPower, kToolbarShot ];
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarAllowedItemIdentifiers:(NSToolbar *)toolbar
{
    return [self toolbarDefaultItemIdentifiers:toolbar];
}

- (NSToolbarItem *)toolbar:(NSToolbar *)toolbar
     itemForItemIdentifier:(NSToolbarItemIdentifier)ident
 willBeInsertedIntoToolbar:(BOOL)flag
{
    NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:ident];
    NSString *symbol = nil;
    NSString *label = nil;
    ui_cmd_t cmd = UI_CMD_NONE;

    (void)toolbar;
    (void)flag;
    if ([ident isEqualToString:kToolbarRocket]) {
        symbol = @"square.grid.2x2";
        label = @"Rocket";
        cmd = UI_CMD_ROCKET;
    } else if ([ident isEqualToString:kToolbarPower]) {
        symbol = @"power";
        label = @"Power";
        cmd = UI_CMD_POWER;
    } else if ([ident isEqualToString:kToolbarShot]) {
        symbol = @"camera";
        label = @"Screenshot";
        cmd = UI_CMD_SCREENSHOT;
    } else {
        return nil;
    }
    item.label = label;
    item.paletteLabel = label;
    item.toolTip = label;
    item.tag = cmd;
    item.target = self;
    item.action = @selector(perform:);
    if (@available(macOS 11.0, *)) {
        item.image = [NSImage imageWithSystemSymbolName:symbol
                               accessibilityDescription:label];
        item.bordered = YES;
    }
    return item;
}

@end

static BE300MenuTarget *menu_target;
static bool menus_installed;
static int shadow_frames_left;

static NSMenuItem *make_item(const ui_menu_item_t *it)
{
    NSString *key = @"";
    NSMenuItem *mi;
    NSEventModifierFlags mask = 0;

    if (it->key)
        key = [NSString stringWithFormat:@"%c", it->key];
    mi = [[NSMenuItem alloc] initWithTitle:[NSString stringWithUTF8String:it->title]
                                    action:@selector(perform:)
                             keyEquivalent:key];
    if (it->mods & UI_MOD_CMD)
        mask |= NSEventModifierFlagCommand;
    if (it->mods & UI_MOD_SHIFT)
        mask |= NSEventModifierFlagShift;
    if (it->mods & UI_MOD_ALT)
        mask |= NSEventModifierFlagOption;
    mi.keyEquivalentModifierMask = mask;
    mi.target = menu_target;
    mi.tag = it->cmd;
    return mi;
}

static bool item_visible(const ui_menu_item_t *it, bool context)
{
    if (it->flags & UI_ITEM_NOT_MAC)
        return false;
    if ((it->flags & UI_ITEM_EMULATOR) && !ui_cmd_emulator_active())
        return false;
    if (context && !(it->flags & UI_ITEM_CONTEXT) && it->cmd != UI_CMD_NONE)
        return false;
    return true;
}

/* Fill `menu` with the table entries for `which`; D-pad entries go into a
 * submenu. Leading, trailing and doubled separators are dropped. */
static void fill_menu(NSMenu *menu, ui_menu_t which, bool context)
{
    NSMenu *dpad = nil;
    bool pending_sep = false;

    for (size_t i = 0; i < ui_menu_item_count; i++) {
        const ui_menu_item_t *it = &ui_menu_items[i];

        if (it->menu != which || !item_visible(it, context))
            continue;
        if (it->cmd == UI_CMD_NONE) {
            pending_sep = menu.numberOfItems > 0;
            continue;
        }
        if (it->submenu_of != UI_MENU_COUNT) {
            if (context)
                continue;
            if (!dpad) {
                NSMenuItem *holder = [[NSMenuItem alloc]
                    initWithTitle:@UI_DPAD_SUBMENU_TITLE action:nil keyEquivalent:@""];
                dpad = [[NSMenu alloc] initWithTitle:@UI_DPAD_SUBMENU_TITLE];
                holder.submenu = dpad;
                if (pending_sep)
                    [menu addItem:[NSMenuItem separatorItem]];
                pending_sep = false;
                [menu addItem:holder];
            }
            [dpad addItem:make_item(it)];
            continue;
        }
        if (pending_sep)
            [menu addItem:[NSMenuItem separatorItem]];
        pending_sep = false;
        [menu addItem:make_item(it)];
    }
}

static NSMenuItem *top_level(NSString *title, NSMenu *sub)
{
    NSMenuItem *holder = [[NSMenuItem alloc] initWithTitle:title action:nil
                                             keyEquivalent:@""];
    sub.title = title;
    holder.submenu = sub;
    return holder;
}

void ui_platform_sdl_hints(void)
{
    /* Cmd+click stays a click; SDL must not turn ctrl-click into right. */
    SDL_SetHint(SDL_HINT_MAC_CTRL_CLICK_EMULATE_RIGHT_CLICK, "0");
}

void ui_platform_install_menus(void)
{
    NSMenu *bar, *app, *menu;
    NSMenuItem *mi;

    if (!cocoa_video())
        return;
    if (!menu_target)
        menu_target = [[BE300MenuTarget alloc] init];

    bar = [[NSMenu alloc] initWithTitle:@"MainMenu"];

    app = [[NSMenu alloc] initWithTitle:@"BE-300"];
    fill_menu(app, UI_MENU_APP, false);
    /* Standard application items AppKit implements itself, between
     * About + separator and Quit. */
    mi =[[NSMenuItem alloc] initWithTitle:@"Hide BE-300 Emulator"
                                    action:@selector(hide:) keyEquivalent:@"h"];
    [app insertItem:mi atIndex:app.numberOfItems - 1];
    mi = [[NSMenuItem alloc] initWithTitle:@"Hide Others"
                                    action:@selector(hideOtherApplications:)
                             keyEquivalent:@"h"];
    mi.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [app insertItem:mi atIndex:app.numberOfItems - 1];
    mi = [[NSMenuItem alloc] initWithTitle:@"Show All"
                                    action:@selector(unhideAllApplications:)
                             keyEquivalent:@""];
    [app insertItem:mi atIndex:app.numberOfItems - 1];
    [app insertItem:[NSMenuItem separatorItem] atIndex:app.numberOfItems - 1];
    [bar addItem:top_level(@"BE-300", app)];

    for (int m = UI_MENU_FILE; m < UI_MENU_COUNT; m++) {
        menu = [[NSMenu alloc] initWithTitle:@""];
        fill_menu(menu, (ui_menu_t)m, false);
        if (m == UI_MENU_WINDOW) {
            mi = [[NSMenuItem alloc] initWithTitle:@"Bring All to Front"
                                            action:@selector(arrangeInFront:)
                                     keyEquivalent:@""];
            [menu addItem:[NSMenuItem separatorItem]];
            [menu addItem:mi];
        }
        if (menu.numberOfItems == 0)
            continue;
        [bar addItem:top_level([NSString stringWithUTF8String:ui_menu_titles[m]], menu)];
        if (m == UI_MENU_WINDOW)
            NSApp.windowsMenu = menu;
        if (m == UI_MENU_HELP)
            NSApp.helpMenu = menu;
    }

    NSApp.mainMenu = bar;
    menus_installed = true;
}

bool ui_platform_menu_handles_shortcuts(void)
{
    return menus_installed && cocoa_video();
}

void ui_platform_attach_window(SDL_Window *win, bool bezel,
    const char *title, const char *subtitle, double aspect)
{
    NSWindow *w = ns_window_for(win);
    int cw = 0, ch = 0;

    if (!w)
        return;
    SDL_GetWindowSize(win, &cw, &ch);
    w.title = [NSString stringWithUTF8String:title ? title : "BE-300"];
    if (@available(macOS 11.0, *))
        w.subtitle = subtitle ? [NSString stringWithUTF8String:subtitle] : @"";
    w.tabbingMode = NSWindowTabbingModeDisallowed;

    if (bezel) {
        w.styleMask |= NSWindowStyleMaskMiniaturizable;
        w.hasShadow = YES;
        shadow_frames_left = 3;
    } else if (ui_cmd_emulator_active()) {
        NSToolbar *tb = [[NSToolbar alloc] initWithIdentifier:@"be300.toolbar"];
        tb.delegate = menu_target ? menu_target :
            (menu_target = [[BE300MenuTarget alloc] init]);
        tb.displayMode = NSToolbarDisplayModeIconOnly;
        tb.allowsUserCustomization = NO;
        w.toolbar = tb;
        if (@available(macOS 11.0, *))
            w.toolbarStyle = NSWindowToolbarStyleUnifiedCompact;
        /* Adding the toolbar must not shrink the device content. */
        if (cw > 0 && ch > 0)
            SDL_SetWindowSize(win, cw, ch);
    }
    ui_platform_set_aspect(win, aspect);
}

void ui_platform_detach_window(SDL_Window *win)
{
    NSWindow *w = ns_window_for(win);

    if (w)
        w.toolbar = nil;
}

void ui_platform_set_aspect(SDL_Window *win, double aspect)
{
    NSWindow *w = ns_window_for(win);

    if (w && aspect > 0.0)
        w.contentAspectRatio = NSMakeSize(aspect * 1000.0, 1000.0);
}

void ui_platform_frame_presented(SDL_Window *win, bool bezel)
{
    NSWindow *w;

    if (!bezel || shadow_frames_left <= 0)
        return;
    shadow_frames_left--;
    w = ns_window_for(win);
    [w invalidateShadow];
}

void ui_platform_make_window_transparent(SDL_Window *win)
{
    NSWindow *w = ns_window_for(win);
    NSView *view;

    if (!w)
        return;
    w.opaque = NO;
    w.backgroundColor = [NSColor clearColor];
    view = w.contentView;
    view.wantsLayer = YES;
    view.layer.opaque = NO;
    view.layer.backgroundColor = [NSColor clearColor].CGColor;
}

static void release_cg_data(void *info, const void *data, size_t size)
{
    (void)info;
    (void)size;
    free((void *)data);
}

void ui_platform_apply_window_mask(SDL_Window *win, const uint8_t *mask,
    int mask_w, int mask_h)
{
    NSWindow *w = ns_window_for(win);
    NSView *view;
    CALayer *mask_layer;
    CGColorSpaceRef cs;
    CGDataProviderRef provider;
    CGImageRef image;
    uint8_t *rgba;
    size_t n;

    if (!w || !mask || mask_w <= 0 || mask_h <= 0)
        return;
    view = w.contentView;
    view.wantsLayer = YES;

    n = (size_t)mask_w * (size_t)mask_h;
    rgba = malloc(n * 4u);
    if (!rgba)
        return;
    for (size_t i = 0; i < n; i++) {
        rgba[i * 4u + 0u] = 255;
        rgba[i * 4u + 1u] = 255;
        rgba[i * 4u + 2u] = 255;
        rgba[i * 4u + 3u] = mask[i];
    }
    cs = CGColorSpaceCreateDeviceRGB();
    provider = CGDataProviderCreateWithData(NULL, rgba, n * 4u, release_cg_data);
    if (!provider) {
        free(rgba);
        CGColorSpaceRelease(cs);
        return;
    }
    image = CGImageCreate((size_t)mask_w, (size_t)mask_h, 8, 32,
        (size_t)mask_w * 4u, cs, kCGBitmapByteOrder32Big | kCGImageAlphaLast,
        provider, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(cs);
    if (!image)
        return;

    mask_layer = [CALayer layer];
    mask_layer.frame = view.layer.bounds;
    mask_layer.contents = (__bridge id)image;
    mask_layer.contentsGravity = kCAGravityResize;
    CGImageRelease(image);
    view.layer.masksToBounds = YES;
    view.layer.mask = mask_layer;
    w.hasShadow = YES;
    shadow_frames_left = 3;
}

void ui_platform_context_menu(SDL_Window *win, int x, int y)
{
    NSWindow *w = ns_window_for(win);
    NSView *view;
    NSMenu *menu;
    NSPoint p;

    if (!w)
        return;
    if (!menu_target)
        menu_target = [[BE300MenuTarget alloc] init];
    menu = [[NSMenu alloc] initWithTitle:@"BE-300"];
    for (int m = UI_MENU_FILE; m < UI_MENU_COUNT; m++) {
        NSInteger before = menu.numberOfItems;
        if (before > 0)
            [menu addItem:[NSMenuItem separatorItem]];
        fill_menu(menu, (ui_menu_t)m, true);
        if (menu.numberOfItems == before + (before > 0 ? 1 : 0) && before > 0)
            [menu removeItemAtIndex:before];
    }
    view = w.contentView;
    p = NSMakePoint(x, view.isFlipped ? y : view.bounds.size.height - y);
    [menu popUpMenuPositioningItem:nil atLocation:p inView:view];
}

void ui_platform_about(SDL_Window *win)
{
    NSDictionary *opts;

    (void)win;
    if (!cocoa_video())
        return;
    opts = @{
        NSAboutPanelOptionApplicationName : @"BE-300 Emulator",
        NSAboutPanelOptionApplicationVersion : @BE300_VERSION_STRING,
        NSAboutPanelOptionVersion : @"",
        NSAboutPanelOptionCredits : [[NSAttributedString alloc]
            initWithString:@"Casio CASSIOPEIA BE-300 (NEC VR4131) emulator.\n"
                           "Boots Windows CE 3.0 from the real boot ROM."],
    };
    [NSApp orderFrontStandardAboutPanelWithOptions:opts];
    [NSApp activateIgnoringOtherApps:YES];
}

bool ui_platform_open_file(SDL_Window *win, const char *title,
    const char *extension, char *out, size_t cap)
{
    NSOpenPanel *panel;
    NSWindow *w = ns_window_for(win);
    bool ok = false;

    if (!cocoa_video() || !out || cap == 0)
        return false;
    panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.message = title ? [NSString stringWithUTF8String:title] : @"";
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (extension)
        panel.allowedFileTypes = @[ [NSString stringWithUTF8String:extension] ];
#pragma clang diagnostic pop
    if ([panel runModal] == NSModalResponseOK && panel.URL) {
        const char *path = panel.URL.fileSystemRepresentation;
        if (path && strlen(path) < cap) {
            strcpy(out, path);
            ok = true;
        }
    }
    [w makeKeyAndOrderFront:nil];
    return ok;
}

bool ui_platform_copy_image(const uint8_t *rgba, int w, int h)
{
    NSBitmapImageRep *rep;
    NSPasteboard *pb;
    NSData *png, *tiff;

    if (!rgba || w <= 0 || h <= 0)
        return false;
    rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:NULL pixelsWide:w pixelsHigh:h
                   bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
                  colorSpaceName:NSDeviceRGBColorSpace
                    bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
                     bytesPerRow:(NSInteger)w * 4 bitsPerPixel:32];
    if (!rep)
        return false;
    memcpy(rep.bitmapData, rgba, (size_t)w * (size_t)h * 4u);
    png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    tiff = rep.TIFFRepresentation;
    pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    if (png)
        [pb setData:png forType:NSPasteboardTypePNG];
    if (tiff)
        [pb setData:tiff forType:NSPasteboardTypeTIFF];
    return png || tiff;
}

bool ui_platform_screenshot_dir(char *out, size_t cap)
{
    NSString *dir = NSSearchPathForDirectoriesInDomains(NSDesktopDirectory,
        NSUserDomainMask, YES).firstObject;
    const char *s = dir.fileSystemRepresentation;

    if (!s || strlen(s) >= cap)
        return false;
    strcpy(out, s);
    return true;
}
