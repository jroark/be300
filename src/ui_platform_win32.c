/*
 *  ui_platform_win32.c — Win32 integration for the SDL frontend: an HMENU
 *  menu bar on the framed window, a TrackPopupMenu context menu on the
 *  borderless bezel window, aspect-locked sizing (WM_SIZING), a dark title
 *  bar that follows the system theme, the clipboard and the common open
 *  dialog. See ui_platform.h.
 *
 *  Menu shortcuts are handled by the SDL key handler (src/ui.c) rather than
 *  an accelerator table, so they also work on the borderless bezel window.
 */
#include "ui_platform.h"
#include "ui_commands.h"

#ifdef HAVE_SDL2

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <SDL_syswm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BE300_VERSION_STRING
#define BE300_VERSION_STRING "0.1.0"
#endif

#define CMD_ID_BASE   0x4000u
#define SUBCLASS_ID   0xBE300u

typedef struct {
    double aspect;
    bool   bezel;
    HMENU  bar;
} win_state_t;

static HWND hwnd_for(SDL_Window *win)
{
    SDL_SysWMinfo info;

    if (!win)
        return NULL;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS)
        return NULL;
    return info.info.win.window;
}

static wchar_t *utf8_to_wide(const char *s)
{
    int n;
    wchar_t *w;

    if (!s)
        s = "";
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    w = malloc((size_t)(n > 0 ? n : 1) * sizeof(*w));
    if (!w)
        return NULL;
    if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n))
        w[0] = 0;
    return w;
}

static bool item_visible(const ui_menu_item_t *it, bool context)
{
    if (it->flags & UI_ITEM_MAC_ONLY)
        return false;
    if ((it->flags & UI_ITEM_EMULATOR) && !ui_cmd_emulator_active())
        return false;
    if (context && it->cmd != UI_CMD_NONE && !(it->flags & UI_ITEM_CONTEXT))
        return false;
    return true;
}

static void append_item(HMENU menu, const ui_menu_item_t *it)
{
    char label[64], text[160];
    wchar_t *w;

    ui_cmd_shortcut_label(it, false, label, sizeof(label));
    if (label[0])
        snprintf(text, sizeof(text), "%s\t%s", it->title, label);
    else
        snprintf(text, sizeof(text), "%s", it->title);
    w = utf8_to_wide(text);
    AppendMenuW(menu, MF_STRING, CMD_ID_BASE + (UINT)it->cmd, w ? w : L"?");
    free(w);
}

static void fill_menu(HMENU menu, ui_menu_t which, bool context)
{
    HMENU dpad = NULL;
    bool pending_sep = false;
    int count = 0;

    for (size_t i = 0; i < ui_menu_item_count; i++) {
        const ui_menu_item_t *it = &ui_menu_items[i];

        if (it->menu != which || !item_visible(it, context))
            continue;
        if (it->cmd == UI_CMD_NONE) {
            pending_sep = GetMenuItemCount(menu) > 0;
            continue;
        }
        if (it->submenu_of != UI_MENU_COUNT) {
            if (context)
                continue;
            if (!dpad) {
                dpad = CreatePopupMenu();
                if (pending_sep)
                    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
                pending_sep = false;
                AppendMenuW(menu, MF_POPUP, (UINT_PTR)dpad, L"D-&pad");
            }
            append_item(dpad, it);
            continue;
        }
        if (pending_sep)
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        pending_sep = false;
        append_item(menu, it);
        count++;
    }
    (void)count;
}

static HMENU build_menu_bar(void)
{
    static const wchar_t *const titles[UI_MENU_COUNT] = {
        L"", L"&File", L"&Device", L"&View", L"&Window", L"&Help"
    };
    HMENU bar = CreateMenu();

    for (int m = UI_MENU_FILE; m < UI_MENU_COUNT; m++) {
        HMENU sub = CreatePopupMenu();
        fill_menu(sub, (ui_menu_t)m, false);
        if (GetMenuItemCount(sub) <= 0) {
            DestroyMenu(sub);
            continue;
        }
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)sub, titles[m]);
    }
    return bar;
}

static void refresh_states(HMENU menu)
{
    int n = GetMenuItemCount(menu);

    for (int i = 0; i < n; i++) {
        UINT id = GetMenuItemID(menu, i);
        HMENU sub = GetSubMenu(menu, i);

        if (sub) {
            refresh_states(sub);
            continue;
        }
        if (id == (UINT)-1 || id < CMD_ID_BASE ||
            id >= CMD_ID_BASE + UI_CMD_COUNT)
            continue;
        EnableMenuItem(menu, id, MF_BYCOMMAND |
            (ui_cmd_enabled((ui_cmd_t)(id - CMD_ID_BASE)) ? MF_ENABLED : MF_GRAYED));
        CheckMenuItem(menu, id, MF_BYCOMMAND |
            (ui_cmd_checked((ui_cmd_t)(id - CMD_ID_BASE)) ? MF_CHECKED : MF_UNCHECKED));
    }
}

/* Keep the client area at the content's aspect ratio while dragging. */
static void constrain_sizing(HWND hwnd, const win_state_t *st, WPARAM edge,
    RECT *r)
{
    RECT frame = { 0, 0, 0, 0 };
    LONG dx, dy, cw, ch;
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    LONG exstyle = GetWindowLongW(hwnd, GWL_EXSTYLE);

    if (st->aspect <= 0.0)
        return;
    AdjustWindowRectEx(&frame, (DWORD)style, GetMenu(hwnd) != NULL,
        (DWORD)exstyle);
    dx = frame.right - frame.left;
    dy = frame.bottom - frame.top;
    cw = (r->right - r->left) - dx;
    ch = (r->bottom - r->top) - dy;
    if (cw < 1)
        cw = 1;
    if (ch < 1)
        ch = 1;

    switch (edge) {
    case WMSZ_LEFT:
    case WMSZ_RIGHT:
        r->bottom = r->top + dy + (LONG)(cw / st->aspect + 0.5);
        break;
    case WMSZ_TOP:
    case WMSZ_BOTTOM:
        r->right = r->left + dx + (LONG)(ch * st->aspect + 0.5);
        break;
    case WMSZ_TOPLEFT:
        r->top = r->bottom - dy - (LONG)(cw / st->aspect + 0.5);
        break;
    case WMSZ_TOPRIGHT:
        r->top = r->bottom - dy - (LONG)(cw / st->aspect + 0.5);
        break;
    case WMSZ_BOTTOMLEFT:
    case WMSZ_BOTTOMRIGHT:
    default:
        r->bottom = r->top + dy + (LONG)(cw / st->aspect + 0.5);
        break;
    }
}

static LRESULT CALLBACK subclass_proc(HWND hwnd, UINT msg, WPARAM wp,
    LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    win_state_t *st = (win_state_t *)ref;

    switch (msg) {
    case WM_COMMAND:
        if (HIWORD(wp) == 0 && LOWORD(wp) >= CMD_ID_BASE &&
            LOWORD(wp) < CMD_ID_BASE + UI_CMD_COUNT) {
            ui_cmd_post((ui_cmd_t)(LOWORD(wp) - CMD_ID_BASE));
            return 0;
        }
        break;
    case WM_INITMENUPOPUP:
        refresh_states((HMENU)wp);
        break;
    case WM_SIZING:
        constrain_sizing(hwnd, st, wp, (RECT *)lp);
        return TRUE;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, subclass_proc, id);
        if (st && st->bar)
            DestroyMenu(st->bar);
        free(st);
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static win_state_t *state_for(HWND hwnd)
{
    DWORD_PTR ref = 0;

    if (!hwnd || !GetWindowSubclass(hwnd, subclass_proc, SUBCLASS_ID, &ref))
        return NULL;
    return (win_state_t *)ref;
}

static void apply_dark_title_bar(HWND hwnd)
{
    typedef HRESULT (WINAPI *dwm_set_fn)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwm;
    dwm_set_fn set;
    DWORD light = 1, size = sizeof(light);
    BOOL dark;

    if (RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size) !=
        ERROR_SUCCESS)
        return;
    dark = light == 0;
    dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm)
        return;
    set = (dwm_set_fn)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
    /* DWMWA_USE_IMMERSIVE_DARK_MODE: 20 on Windows 10 20H1+, 19 before. */
    if (set && FAILED(set(hwnd, 20, &dark, sizeof(dark))))
        set(hwnd, 19, &dark, sizeof(dark));
    FreeLibrary(dwm);
}

void ui_platform_sdl_hints(void)
{
    /* SDL >= 2.24: per-monitor DPI awareness with window sizes in points,
     * so the layout code works in the same units as on macOS. */
    SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");
    SDL_SetHint("SDL_WINDOWS_DPI_SCALING", "1");
    /* Window and taskbar icon from the executable's resource 1. */
    SDL_SetHint("SDL_WINDOWS_INTRESOURCE_ICON", "1");
    SDL_SetHint("SDL_WINDOWS_INTRESOURCE_ICON_SMALL", "1");
}

void ui_platform_install_menus(void) {}

bool ui_platform_menu_handles_shortcuts(void)
{
    return false;
}

void ui_platform_attach_window(SDL_Window *win, bool bezel,
    const char *title, const char *subtitle, double aspect)
{
    HWND hwnd = hwnd_for(win);
    win_state_t *st;
    char buf[512];
    int w = 0, h = 0;

    if (subtitle && *subtitle)
        snprintf(buf, sizeof(buf), "%s \xE2\x80\x94 %s", title ? title : "BE-300",
            subtitle);
    else
        snprintf(buf, sizeof(buf), "%s", title ? title : "BE-300");
    SDL_SetWindowTitle(win, buf);
    if (!hwnd)
        return;

    st = state_for(hwnd);
    if (!st) {
        st = calloc(1, sizeof(*st));
        if (!st)
            return;
        if (!SetWindowSubclass(hwnd, subclass_proc, SUBCLASS_ID, (DWORD_PTR)st)) {
            free(st);
            return;
        }
    }
    st->aspect = aspect;
    st->bezel = bezel;

    if (!bezel) {
        SDL_GetWindowSize(win, &w, &h);
        st->bar = build_menu_bar();
        SetMenu(hwnd, st->bar);
        DrawMenuBar(hwnd);
        /* SDL sizes the frame for the menu; keep the client area. */
        if (w > 0 && h > 0)
            SDL_SetWindowSize(win, w, h);
        apply_dark_title_bar(hwnd);
    }
}

void ui_platform_detach_window(SDL_Window *win)
{
    HWND hwnd = hwnd_for(win);
    win_state_t *st = state_for(hwnd);

    if (st && st->bar) {
        SetMenu(hwnd, NULL);
        DestroyMenu(st->bar);
        st->bar = NULL;
    }
}

void ui_platform_set_aspect(SDL_Window *win, double aspect)
{
    win_state_t *st = state_for(hwnd_for(win));

    if (st)
        st->aspect = aspect;
}

void ui_platform_frame_presented(SDL_Window *win, bool bezel)
{
    (void)win;
    (void)bezel;
}

/* The bezel window is shaped with SetWindowRgn by SDL_SetWindowShape. */
void ui_platform_make_window_transparent(SDL_Window *win) { (void)win; }
void ui_platform_apply_window_mask(SDL_Window *win, const uint8_t *mask,
    int mask_w, int mask_h)
{ (void)win; (void)mask; (void)mask_w; (void)mask_h; }

static double window_dpi_scale(HWND hwnd)
{
    typedef UINT (WINAPI *get_dpi_fn)(HWND);
    HMODULE user = GetModuleHandleW(L"user32.dll");
    get_dpi_fn get = user ?
        (get_dpi_fn)(void *)GetProcAddress(user, "GetDpiForWindow") : NULL;
    UINT dpi = get ? get(hwnd) : 96;

    return dpi ? dpi / 96.0 : 1.0;
}

void ui_platform_context_menu(SDL_Window *win, int x, int y)
{
    HWND hwnd = hwnd_for(win);
    HMENU menu;
    POINT p;
    double s;
    UINT cmd;

    if (!hwnd)
        return;
    menu = CreatePopupMenu();
    for (int m = UI_MENU_FILE; m < UI_MENU_COUNT; m++) {
        int before = GetMenuItemCount(menu);
        if (before > 0)
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        fill_menu(menu, (ui_menu_t)m, true);
        if (before > 0 && GetMenuItemCount(menu) == before + 1)
            DeleteMenu(menu, (UINT)before, MF_BYPOSITION);
    }
    refresh_states(menu);
    s = window_dpi_scale(hwnd);
    p.x = (LONG)(x * s);
    p.y = (LONG)(y * s);
    ClientToScreen(hwnd, &p);
    cmd = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        p.x, p.y, 0, hwnd, NULL);
    DestroyMenu(menu);
    if (cmd >= CMD_ID_BASE && cmd < CMD_ID_BASE + UI_CMD_COUNT)
        ui_cmd_post((ui_cmd_t)(cmd - CMD_ID_BASE));
}

void ui_platform_about(SDL_Window *win)
{
    MessageBoxW(hwnd_for(win),
        L"BE-300 Emulator " BE300_VERSION_STRING L"\n\n"
        L"Casio CASSIOPEIA BE-300 (NEC VR4131) emulator.\n"
        L"Boots Windows CE 3.0 from the real boot ROM.",
        L"About BE-300 Emulator", MB_OK | MB_ICONINFORMATION);
}

bool ui_platform_open_file(SDL_Window *win, const char *title,
    const char *extension, char *out, size_t cap)
{
    OPENFILENAMEW ofn;
    wchar_t path[MAX_PATH] = L"";
    wchar_t *wtitle = utf8_to_wide(title);
    bool ok;

    (void)extension;  /* the filter below is the only caller's (.cbea) */
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_for(win);
    ofn.lpstrFilter = L"CASIO app packages (*.cbea)\0*.cbea\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = wtitle;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ok = GetOpenFileNameW(&ofn) &&
        WideCharToMultiByte(CP_UTF8, 0, path, -1, out, (int)cap, NULL, NULL) > 0;
    free(wtitle);
    return ok;
}

bool ui_platform_copy_image(const uint8_t *rgba, int w, int h)
{
    size_t row = (size_t)w * 4u;
    size_t bytes = sizeof(BITMAPINFOHEADER) + row * (size_t)h;
    HGLOBAL mem;
    BITMAPINFOHEADER *bi;
    uint8_t *px;

    if (!rgba || w <= 0 || h <= 0 || !OpenClipboard(NULL))
        return false;
    EmptyClipboard();
    mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) {
        CloseClipboard();
        return false;
    }
    bi = GlobalLock(mem);
    memset(bi, 0, sizeof(*bi));
    bi->biSize = sizeof(*bi);
    bi->biWidth = w;
    bi->biHeight = h;  /* bottom-up */
    bi->biPlanes = 1;
    bi->biBitCount = 32;
    bi->biCompression = BI_RGB;
    bi->biSizeImage = (DWORD)(row * (size_t)h);
    px = (uint8_t *)(bi + 1);
    for (int y = 0; y < h; y++) {
        const uint8_t *src = rgba + (size_t)(h - 1 - y) * row;
        uint8_t *dst = px + (size_t)y * row;
        for (int x = 0; x < w; x++) {
            /* Composite over white: most consumers ignore DIB alpha. */
            unsigned a = src[x * 4 + 3];
            dst[x * 4 + 0] = (uint8_t)((src[x * 4 + 2] * a + 255u * (255u - a)) / 255u);
            dst[x * 4 + 1] = (uint8_t)((src[x * 4 + 1] * a + 255u * (255u - a)) / 255u);
            dst[x * 4 + 2] = (uint8_t)((src[x * 4 + 0] * a + 255u * (255u - a)) / 255u);
            dst[x * 4 + 3] = 255;
        }
    }
    GlobalUnlock(mem);
    if (!SetClipboardData(CF_DIB, mem)) {
        GlobalFree(mem);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

bool ui_platform_screenshot_dir(char *out, size_t cap)
{
    wchar_t path[MAX_PATH];

    if (FAILED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, path)))
        return false;
    return WideCharToMultiByte(CP_UTF8, 0, path, -1, out, (int)cap, NULL,
        NULL) > 0;
}

#endif /* HAVE_SDL2 */
