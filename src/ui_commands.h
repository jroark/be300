/*
 *  ui_commands.h — desktop frontend commands shared by the native menu
 *  bars (macOS NSMenu, Windows HMENU), the bezel context menu and the
 *  keyboard shortcut handler. Every source posts the same command into one
 *  queue that src/ui.c drains, so a menu click and its shortcut behave the
 *  same.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    UI_CMD_NONE = 0,
    UI_CMD_ABOUT,
    UI_CMD_QUIT,
    UI_CMD_INSTALL_APP,
    UI_CMD_SCREENSHOT,
    UI_CMD_COPY_SCREEN,
    UI_CMD_CLOSE,
    UI_CMD_ROCKET,
    UI_CMD_POWER,
    UI_CMD_OK,
    UI_CMD_ESC,
    UI_CMD_UP,
    UI_CMD_DOWN,
    UI_CMD_LEFT,
    UI_CMD_RIGHT,
    UI_CMD_RESTART,
    UI_CMD_BEZEL,
    UI_CMD_ZOOM_1,
    UI_CMD_ZOOM_2,
    UI_CMD_ZOOM_3,
    UI_CMD_ZOOM_4,
    UI_CMD_FIT,
    UI_CMD_PIXEL_ACCURATE,
    UI_CMD_MINIMIZE,
    UI_CMD_SHORTCUTS,
    UI_CMD_COUNT
} ui_cmd_t;

typedef enum {
    UI_MENU_APP = 0,    /* macOS application menu; folded into File/Help elsewhere */
    UI_MENU_FILE,
    UI_MENU_DEVICE,
    UI_MENU_VIEW,
    UI_MENU_WINDOW,
    UI_MENU_HELP,
    UI_MENU_COUNT
} ui_menu_t;

/* Shortcut modifiers. UI_MOD_CMD is Command on macOS and Ctrl elsewhere. */
#define UI_MOD_CMD   0x1u
#define UI_MOD_SHIFT 0x2u
#define UI_MOD_ALT   0x4u

#define UI_ITEM_EMULATOR  0x1u  /* only while a machine is running */
#define UI_ITEM_CHECKABLE 0x2u
#define UI_ITEM_CONTEXT   0x4u  /* also in the bezel's right-click menu */
#define UI_ITEM_MAC_ONLY  0x8u
#define UI_ITEM_NOT_MAC   0x10u

typedef struct {
    ui_menu_t   menu;
    ui_cmd_t    cmd;         /* UI_CMD_NONE: separator */
    const char *title;
    char        key;         /* lower-case key equivalent, 0 for none */
    unsigned    mods;
    unsigned    flags;
    ui_menu_t   submenu_of;  /* D-pad items sit in a submenu; UI_MENU_COUNT if not */
} ui_menu_item_t;

extern const ui_menu_item_t ui_menu_items[];
extern const size_t ui_menu_item_count;
extern const char *const ui_menu_titles[UI_MENU_COUNT];

/* Title of the D-pad submenu inside Device. */
#define UI_DPAD_SUBMENU_TITLE "D-pad"

void ui_cmd_post(ui_cmd_t cmd);
bool ui_cmd_next(ui_cmd_t *cmd_out);
void ui_cmd_clear(void);

/* Menu item state, read by the platform menus when they open. */
void ui_cmd_set_checked(ui_cmd_t cmd, bool checked);
bool ui_cmd_checked(ui_cmd_t cmd);
void ui_cmd_set_enabled(ui_cmd_t cmd, bool enabled);
bool ui_cmd_enabled(ui_cmd_t cmd);

/* True while an emulated machine owns the window; hides Device/View. */
void ui_cmd_set_emulator_active(bool active);
bool ui_cmd_emulator_active(void);

/* Shortcut lookup: key is the lower-case character. */
ui_cmd_t ui_cmd_for_shortcut(char key, unsigned mods);

/* Human-readable shortcut ("Cmd+S" / "Ctrl+S"); empty when none. */
void ui_cmd_shortcut_label(const ui_menu_item_t *item, bool mac,
    char *out, size_t cap);

/* Multi-line help text listing every shortcut and hardware key. */
void ui_cmd_help_text(bool mac, char *out, size_t cap);
