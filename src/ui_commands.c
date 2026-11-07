/*
 *  ui_commands.c — command queue, menu table and shortcuts (ui_commands.h).
 */
#include "ui_commands.h"

#include <stdio.h>
#include <string.h>

#define NOSUB UI_MENU_COUNT

const char *const ui_menu_titles[UI_MENU_COUNT] = {
    "BE-300", "File", "Device", "View", "Window", "Help"
};

const ui_menu_item_t ui_menu_items[] = {
    { UI_MENU_APP,    UI_CMD_ABOUT,       "About BE-300 Emulator", 0,   0, UI_ITEM_MAC_ONLY, NOSUB },
    { UI_MENU_APP,    UI_CMD_NONE,        NULL,                    0,   0, UI_ITEM_MAC_ONLY, NOSUB },
    { UI_MENU_APP,    UI_CMD_QUIT,        "Quit BE-300 Emulator",  'q', UI_MOD_CMD, UI_ITEM_MAC_ONLY, NOSUB },

    { UI_MENU_FILE,   UI_CMD_INSTALL_APP, "Install App (.cbea)...", 'o', UI_MOD_CMD, UI_ITEM_EMULATOR, NOSUB },
    { UI_MENU_FILE,   UI_CMD_NONE,        NULL,                    0,   0, UI_ITEM_EMULATOR, NOSUB },
    { UI_MENU_FILE,   UI_CMD_SCREENSHOT,  "Save Screenshot",       's', UI_MOD_CMD, UI_ITEM_EMULATOR | UI_ITEM_CONTEXT, NOSUB },
    { UI_MENU_FILE,   UI_CMD_COPY_SCREEN, "Copy Screen",           'c', UI_MOD_CMD, UI_ITEM_EMULATOR | UI_ITEM_CONTEXT, NOSUB },
    { UI_MENU_FILE,   UI_CMD_NONE,        NULL,                    0,   0, UI_ITEM_EMULATOR, NOSUB },
    { UI_MENU_FILE,   UI_CMD_CLOSE,       "Close Window",          'w', UI_MOD_CMD, 0, NOSUB },
    { UI_MENU_FILE,   UI_CMD_QUIT,        "Exit",                  0,   0, UI_ITEM_NOT_MAC, NOSUB },

    { UI_MENU_DEVICE, UI_CMD_ROCKET,      "Rocket",                'h', UI_MOD_CMD | UI_MOD_SHIFT, UI_ITEM_EMULATOR | UI_ITEM_CONTEXT, NOSUB },
    { UI_MENU_DEVICE, UI_CMD_POWER,       "Power",                 'l', UI_MOD_CMD, UI_ITEM_EMULATOR | UI_ITEM_CONTEXT, NOSUB },
    { UI_MENU_DEVICE, UI_CMD_OK,          "OK",                    0,   0, UI_ITEM_EMULATOR | UI_ITEM_CONTEXT, NOSUB },
    { UI_MENU_DEVICE, UI_CMD_ESC,         "Esc",                   0,   0, UI_ITEM_EMULATOR | UI_ITEM_CONTEXT, NOSUB },
    { UI_MENU_DEVICE, UI_CMD_UP,          "Up",                    0,   0, UI_ITEM_EMULATOR, UI_MENU_DEVICE },
    { UI_MENU_DEVICE, UI_CMD_DOWN,        "Down",                  0,   0, UI_ITEM_EMULATOR, UI_MENU_DEVICE },
    { UI_MENU_DEVICE, UI_CMD_LEFT,        "Left",                  0,   0, UI_ITEM_EMULATOR, UI_MENU_DEVICE },
    { UI_MENU_DEVICE, UI_CMD_RIGHT,       "Right",                 0,   0, UI_ITEM_EMULATOR, UI_MENU_DEVICE },
    { UI_MENU_DEVICE, UI_CMD_NONE,        NULL,                    0,   0, UI_ITEM_EMULATOR, NOSUB },
    { UI_MENU_DEVICE, UI_CMD_RESTART,     "Restart (Cold Boot)",   'r', UI_MOD_CMD | UI_MOD_ALT, UI_ITEM_EMULATOR | UI_ITEM_CONTEXT, NOSUB },

    { UI_MENU_VIEW,   UI_CMD_BEZEL,       "Show Bezel",            'b', UI_MOD_CMD, UI_ITEM_EMULATOR | UI_ITEM_CHECKABLE | UI_ITEM_CONTEXT, NOSUB },
    { UI_MENU_VIEW,   UI_CMD_NONE,        NULL,                    0,   0, UI_ITEM_EMULATOR, NOSUB },
    { UI_MENU_VIEW,   UI_CMD_ZOOM_1,      "Zoom 100%",             '1', UI_MOD_CMD, UI_ITEM_EMULATOR | UI_ITEM_CHECKABLE | UI_ITEM_CONTEXT, NOSUB },
    { UI_MENU_VIEW,   UI_CMD_ZOOM_2,      "Zoom 200%",             '2', UI_MOD_CMD, UI_ITEM_EMULATOR | UI_ITEM_CHECKABLE | UI_ITEM_CONTEXT, NOSUB },
    { UI_MENU_VIEW,   UI_CMD_ZOOM_3,      "Zoom 300%",             '3', UI_MOD_CMD, UI_ITEM_EMULATOR | UI_ITEM_CHECKABLE | UI_ITEM_CONTEXT, NOSUB },
    { UI_MENU_VIEW,   UI_CMD_ZOOM_4,      "Zoom 400%",             '4', UI_MOD_CMD, UI_ITEM_EMULATOR | UI_ITEM_CHECKABLE | UI_ITEM_CONTEXT, NOSUB },
    { UI_MENU_VIEW,   UI_CMD_FIT,         "Fit Screen",            '0', UI_MOD_CMD, UI_ITEM_EMULATOR | UI_ITEM_CONTEXT, NOSUB },
    { UI_MENU_VIEW,   UI_CMD_NONE,        NULL,                    0,   0, UI_ITEM_EMULATOR, NOSUB },
    { UI_MENU_VIEW,   UI_CMD_PIXEL_ACCURATE, "Pixel Accurate",     'p', UI_MOD_CMD | UI_MOD_SHIFT, UI_ITEM_EMULATOR | UI_ITEM_CHECKABLE, NOSUB },

    { UI_MENU_WINDOW, UI_CMD_MINIMIZE,    "Minimize",              'm', UI_MOD_CMD, 0, NOSUB },

    { UI_MENU_HELP,   UI_CMD_SHORTCUTS,   "Keyboard Shortcuts",    0,   0, UI_ITEM_EMULATOR, NOSUB },
    { UI_MENU_HELP,   UI_CMD_ABOUT,       "About BE-300 Emulator", 0,   0, UI_ITEM_NOT_MAC, NOSUB },
};

const size_t ui_menu_item_count = sizeof(ui_menu_items) / sizeof(ui_menu_items[0]);

#define QUEUE_CAP 32u

static ui_cmd_t queue[QUEUE_CAP];
static unsigned queue_head, queue_len;
static bool checked[UI_CMD_COUNT];
static bool disabled[UI_CMD_COUNT];
static bool emulator_active;

void ui_cmd_post(ui_cmd_t cmd)
{
    if (cmd <= UI_CMD_NONE || cmd >= UI_CMD_COUNT || queue_len == QUEUE_CAP)
        return;
    queue[(queue_head + queue_len++) % QUEUE_CAP] = cmd;
}

bool ui_cmd_next(ui_cmd_t *cmd_out)
{
    if (queue_len == 0)
        return false;
    *cmd_out = queue[queue_head];
    queue_head = (queue_head + 1u) % QUEUE_CAP;
    queue_len--;
    return true;
}

void ui_cmd_clear(void)
{
    queue_head = queue_len = 0;
}

void ui_cmd_set_checked(ui_cmd_t cmd, bool on)
{
    if (cmd > UI_CMD_NONE && cmd < UI_CMD_COUNT)
        checked[cmd] = on;
}

bool ui_cmd_checked(ui_cmd_t cmd)
{
    return cmd > UI_CMD_NONE && cmd < UI_CMD_COUNT && checked[cmd];
}

void ui_cmd_set_enabled(ui_cmd_t cmd, bool on)
{
    if (cmd > UI_CMD_NONE && cmd < UI_CMD_COUNT)
        disabled[cmd] = !on;
}

bool ui_cmd_enabled(ui_cmd_t cmd)
{
    const ui_menu_item_t *it;

    if (cmd <= UI_CMD_NONE || cmd >= UI_CMD_COUNT || disabled[cmd])
        return false;
    for (size_t i = 0; i < ui_menu_item_count; i++) {
        it = &ui_menu_items[i];
        if (it->cmd == cmd && (it->flags & UI_ITEM_EMULATOR) &&
            !emulator_active)
            return false;
    }
    return true;
}

void ui_cmd_set_emulator_active(bool active)
{
    emulator_active = active;
}

bool ui_cmd_emulator_active(void)
{
    return emulator_active;
}

static bool item_on_platform(const ui_menu_item_t *it)
{
#ifdef __APPLE__
    return !(it->flags & UI_ITEM_NOT_MAC);
#else
    return !(it->flags & UI_ITEM_MAC_ONLY);
#endif
}

ui_cmd_t ui_cmd_for_shortcut(char key, unsigned mods)
{
    for (size_t i = 0; i < ui_menu_item_count; i++) {
        const ui_menu_item_t *it = &ui_menu_items[i];
        if (it->cmd != UI_CMD_NONE && it->key && it->key == key &&
            it->mods == mods && item_on_platform(it))
            return it->cmd;
    }
    return UI_CMD_NONE;
}

void ui_cmd_shortcut_label(const ui_menu_item_t *it, bool mac, char *out,
    size_t cap)
{
    char k[2] = { 0, 0 };

    if (!cap)
        return;
    out[0] = '\0';
    if (!it || !it->key)
        return;
    k[0] = (it->key >= 'a' && it->key <= 'z') ?
        (char)(it->key - 'a' + 'A') : it->key;
    snprintf(out, cap, "%s%s%s%s",
        (it->mods & UI_MOD_CMD) ? (mac ? "Cmd+" : "Ctrl+") : "",
        (it->mods & UI_MOD_ALT) ? (mac ? "Option+" : "Alt+") : "",
        (it->mods & UI_MOD_SHIFT) ? "Shift+" : "",
        k);
}

void ui_cmd_help_text(bool mac, char *out, size_t cap)
{
    size_t used = 0;
    char label[32];

#define APPEND(...) do { \
        int n_ = snprintf(out + used, used < cap ? cap - used : 0, __VA_ARGS__); \
        if (n_ > 0) used += (size_t)n_; \
    } while (0)

    if (!cap)
        return;
    out[0] = '\0';
    APPEND("Hardware buttons\n"
        "  Arrows: D-pad    Return: OK    Esc: Esc\n"
        "  Tab: Rocket      F12: Power\n"
        "  Click or drag on the screen or the icon strip below it to tap.\n\n"
        "Shortcuts\n");
    for (size_t i = 0; i < ui_menu_item_count; i++) {
        const ui_menu_item_t *it = &ui_menu_items[i];
        bool dup = false;

        if (it->cmd == UI_CMD_NONE || !it->key || !item_on_platform(it))
            continue;
        for (size_t j = 0; j < i; j++)
            if (ui_menu_items[j].cmd == it->cmd && ui_menu_items[j].key)
                dup = true;
        if (dup)
            continue;
        ui_cmd_shortcut_label(it, mac, label, sizeof(label));
        APPEND("  %-16s %s\n", label, it->title);
    }
    if (!mac)
        APPEND("\nWith the Stowaway keyboard attached, Ctrl goes to the guest;\n"
            "use Ctrl+Shift with the keys above instead.\n");
    APPEND("\nDrop a .cbea package on the window to install it.\n");
#undef APPEND
}
