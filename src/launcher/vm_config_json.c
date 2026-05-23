#include "vm_config_json.h"
#include "vm_bundle.h"
#include "cJSON.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void vm_config_apply_defaults(machine_config_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    /* Mirror main.c's defaults. Anything not set here defaults to 0/NULL/false. */
    cfg->sdram_size            = 16u * 1024u * 1024u;
    /* 0 = unthrottled. The CLI default (main.c) is still 166 MHz for
     * scripted runs; new launcher-created VMs run as fast as the host
     * will go and rely on the WinCE idle loop to keep CPU sane. */
    cfg->target_mhz            = 0u;
    cfg->scale                 = 1.0;
    cfg->stall_window          = 10000u;
    cfg->stall_unique_threshold = 64u;
    cfg->stall_wall_secs       = 5u;
    cfg->pcconnect_baud        = 115200u;
    cfg->pcconnect_dock        = BE300_PCC_DOCK_RS232;
    cfg->serial0_baud          = 115200u;
    cfg->serial1_baud          = 115200u;
}

static const char *dock_to_string(be300_pcconnect_dock_mode_t d)
{
    return (d == BE300_PCC_DOCK_USB_SYNC) ? "usb-sync" : "rs232";
}

static be300_pcconnect_dock_mode_t string_to_dock(const char *s)
{
    if (s && (strcmp(s, "usb-sync") == 0 || strcmp(s, "usb-vcom") == 0))
        return BE300_PCC_DOCK_USB_SYNC;
    return BE300_PCC_DOCK_RS232;
}

static void mac_to_string(const uint8_t mac[6], char buf[18])
{
    snprintf(buf, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int parse_mac_str(const char *s, uint8_t mac[6])
{
    if (!s) return -1;
    unsigned v[6];
    char tail;
    if (sscanf(s, "%x:%x:%x:%x:%x:%x%c",
        &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &tail) != 6) return -1;
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xff) return -1;
        mac[i] = (uint8_t)v[i];
    }
    return 0;
}

static void copy_string_field(char *dst, size_t dst_cap, const cJSON *obj,
    const char *key)
{
    const cJSON *node = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(node) && node->valuestring) {
        snprintf(dst, dst_cap, "%s", node->valuestring);
    } else {
        if (dst_cap > 0) dst[0] = '\0';
    }
}

static bool get_bool(const cJSON *obj, const char *key, bool dflt)
{
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(n)) return cJSON_IsTrue(n) ? true : false;
    return dflt;
}

static double get_number(const cJSON *obj, const char *key, double dflt)
{
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(n)) return n->valuedouble;
    return dflt;
}

static const char *get_string(const cJSON *obj, const char *key)
{
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(n) && n->valuestring) ? n->valuestring : NULL;
}

static char *slurp_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

int vm_config_load(const char *json_path, vm_bundle_t *vm)
{
    if (!json_path || !vm) { errno = EINVAL; return -1; }

    /* Missing config.json is treated as "fresh defaults". The bundle dir
     * exists but the file isn't there yet — most likely just-created. */
    FILE *probe = fopen(json_path, "rb");
    if (!probe) {
        vm_config_apply_defaults(&vm->cfg);
        vm_bundle_rebind_strings(vm);
        return 0;
    }
    fclose(probe);

    size_t len = 0;
    char *text = slurp_file(json_path, &len);
    if (!text) return -1;

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) { errno = EINVAL; return -1; }

    machine_config_t *c = &vm->cfg;
    vm_config_apply_defaults(c);

    c->trace                    = get_bool(root, "trace", false);
    c->log_mmio                 = get_bool(root, "log_mmio", false);
    c->log_nand_legacy          = get_bool(root, "log_nand_legacy", false);
    c->enable_ppsh              = get_bool(root, "enable_ppsh", false);
    c->enable_rtc_host_time     = get_bool(root, "enable_rtc_host_time", false);
    c->enable_stowaway_keyboard = get_bool(root, "enable_stowaway_keyboard", false);
    c->enable_ne2000            = get_bool(root, "enable_ne2000", false);
    c->enable_audio             = get_bool(root, "enable_audio", false);
    c->restore                  = get_bool(root, "restore", false);
    c->mmio_coverage            = get_bool(root, "mmio_coverage", false);
    c->detect_stall             = get_bool(root, "detect_stall", false);
    c->frame_visible            = get_bool(root, "frame_visible", false);

    c->sdram_size               = (uint32_t)get_number(root, "sdram_size", c->sdram_size);
    c->target_mhz               = (uint32_t)get_number(root, "target_mhz", c->target_mhz);
    c->fb_width                 = (uint32_t)get_number(root, "fb_width", 0);
    c->fb_height                = (uint32_t)get_number(root, "fb_height", 0);
    c->fb_stride                = (uint32_t)get_number(root, "fb_stride", 0);
    c->stall_window             = (uint32_t)get_number(root, "stall_window", c->stall_window);
    c->stall_unique_threshold   = (uint32_t)get_number(root, "stall_unique_threshold", c->stall_unique_threshold);
    c->stall_wall_secs          = (uint32_t)get_number(root, "stall_wall_secs", c->stall_wall_secs);
    c->scale                    = get_number(root, "scale", c->scale);
    c->pcconnect_baud           = (uint32_t)get_number(root, "pcconnect_baud", c->pcconnect_baud);
    c->serial0_baud             = (uint32_t)get_number(root, "serial0_baud", c->serial0_baud);
    c->serial1_baud             = (uint32_t)get_number(root, "serial1_baud", c->serial1_baud);

    c->pcconnect_dock           = string_to_dock(get_string(root, "pcconnect_dock"));

    copy_string_field(vm->nand_path_buf,         sizeof vm->nand_path_buf,         root, "nand_path");
    copy_string_field(vm->rom_path_buf,          sizeof vm->rom_path_buf,          root, "rom_path");
    copy_string_field(vm->pcconnect_bridge_buf,  sizeof vm->pcconnect_bridge_buf,  root, "pcconnect_bridge");
    copy_string_field(vm->pcconnect_tee_buf,     sizeof vm->pcconnect_tee_buf,     root, "pcconnect_tee");
    copy_string_field(vm->serial0_bridge_buf,    sizeof vm->serial0_bridge_buf,    root, "serial0_bridge");
    copy_string_field(vm->serial0_tee_buf,       sizeof vm->serial0_tee_buf,       root, "serial0_tee");
    copy_string_field(vm->serial1_bridge_buf,    sizeof vm->serial1_bridge_buf,    root, "serial1_bridge");
    copy_string_field(vm->serial1_tee_buf,       sizeof vm->serial1_tee_buf,       root, "serial1_tee");

    /* CF slots: JSON is an array of strings; entries are placed into
     * cf_path_buf[i] in order. */
    const cJSON *cf_arr = cJSON_GetObjectItemCaseSensitive(root, "cf_paths");
    c->cf_count = 0;
    if (cJSON_IsArray(cf_arr)) {
        int n = cJSON_GetArraySize(cf_arr);
        if (n > (int)BE300_MAX_CF_SLOTS) n = (int)BE300_MAX_CF_SLOTS;
        for (int i = 0; i < n; i++) {
            const cJSON *e = cJSON_GetArrayItem(cf_arr, i);
            if (cJSON_IsString(e) && e->valuestring && e->valuestring[0]) {
                snprintf(vm->cf_path_buf[i], sizeof vm->cf_path_buf[i], "%s",
                    e->valuestring);
                c->cf_count = (unsigned)(i + 1);
            } else {
                vm->cf_path_buf[i][0] = '\0';
            }
        }
    }

    const char *mac_str = get_string(root, "net_mac");
    if (mac_str && parse_mac_str(mac_str, c->net_mac) == 0) {
        c->net_mac_set = true;
    } else {
        c->net_mac_set = false;
    }

    cJSON_Delete(root);
    vm_bundle_rebind_strings(vm);
    return 0;
}

int vm_config_save(const char *json_path, const machine_config_t *c)
{
    if (!json_path || !c) { errno = EINVAL; return -1; }

    cJSON *root = cJSON_CreateObject();
    if (!root) { errno = ENOMEM; return -1; }

    cJSON_AddNumberToObject(root, "schema", BE300_VM_CONFIG_SCHEMA);

    cJSON_AddBoolToObject(root, "trace",                   c->trace);
    cJSON_AddBoolToObject(root, "log_mmio",                c->log_mmio);
    cJSON_AddBoolToObject(root, "log_nand_legacy",         c->log_nand_legacy);
    cJSON_AddBoolToObject(root, "enable_ppsh",             c->enable_ppsh);
    cJSON_AddBoolToObject(root, "enable_rtc_host_time",    c->enable_rtc_host_time);
    cJSON_AddBoolToObject(root, "enable_stowaway_keyboard",c->enable_stowaway_keyboard);
    cJSON_AddBoolToObject(root, "enable_ne2000",           c->enable_ne2000);
    cJSON_AddBoolToObject(root, "enable_audio",            c->enable_audio);
    cJSON_AddBoolToObject(root, "restore",                 c->restore);
    cJSON_AddBoolToObject(root, "mmio_coverage",           c->mmio_coverage);
    cJSON_AddBoolToObject(root, "detect_stall",            c->detect_stall);
    cJSON_AddBoolToObject(root, "frame_visible",           c->frame_visible);

    cJSON_AddNumberToObject(root, "sdram_size",            c->sdram_size);
    cJSON_AddNumberToObject(root, "target_mhz",            c->target_mhz);
    cJSON_AddNumberToObject(root, "fb_width",              c->fb_width);
    cJSON_AddNumberToObject(root, "fb_height",             c->fb_height);
    cJSON_AddNumberToObject(root, "fb_stride",             c->fb_stride);
    cJSON_AddNumberToObject(root, "stall_window",          c->stall_window);
    cJSON_AddNumberToObject(root, "stall_unique_threshold",c->stall_unique_threshold);
    cJSON_AddNumberToObject(root, "stall_wall_secs",       c->stall_wall_secs);
    cJSON_AddNumberToObject(root, "scale",                 c->scale);
    cJSON_AddNumberToObject(root, "pcconnect_baud",        c->pcconnect_baud);
    cJSON_AddNumberToObject(root, "serial0_baud",          c->serial0_baud);
    cJSON_AddNumberToObject(root, "serial1_baud",          c->serial1_baud);

    cJSON_AddStringToObject(root, "pcconnect_dock", dock_to_string(c->pcconnect_dock));

    if (c->nand_path)        cJSON_AddStringToObject(root, "nand_path",        c->nand_path);
    if (c->rom_path)         cJSON_AddStringToObject(root, "rom_path",         c->rom_path);
    if (c->pcconnect_bridge) cJSON_AddStringToObject(root, "pcconnect_bridge", c->pcconnect_bridge);
    if (c->pcconnect_tee)    cJSON_AddStringToObject(root, "pcconnect_tee",    c->pcconnect_tee);
    if (c->serial0_bridge)   cJSON_AddStringToObject(root, "serial0_bridge",   c->serial0_bridge);
    if (c->serial0_tee)      cJSON_AddStringToObject(root, "serial0_tee",      c->serial0_tee);
    if (c->serial1_bridge)   cJSON_AddStringToObject(root, "serial1_bridge",   c->serial1_bridge);
    if (c->serial1_tee)      cJSON_AddStringToObject(root, "serial1_tee",      c->serial1_tee);

    cJSON *cf_arr = cJSON_AddArrayToObject(root, "cf_paths");
    for (unsigned i = 0; i < c->cf_count && i < BE300_MAX_CF_SLOTS; i++) {
        const char *p = c->cf_paths[i] ? c->cf_paths[i] : "";
        cJSON_AddItemToArray(cf_arr, cJSON_CreateString(p));
    }

    if (c->net_mac_set) {
        char mac_str[18];
        mac_to_string(c->net_mac, mac_str);
        cJSON_AddStringToObject(root, "net_mac", mac_str);
    }

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) { errno = ENOMEM; return -1; }

    FILE *f = fopen(json_path, "wb");
    if (!f) { free(text); return -1; }
    size_t len = strlen(text);
    int rc = (fwrite(text, 1, len, f) == len && fputc('\n', f) != EOF) ? 0 : -1;
    if (fclose(f) != 0) rc = -1;
    free(text);
    return rc;
}
