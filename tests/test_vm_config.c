/* Round-trip test for vm_config_save → vm_config_load.
 *
 * Verifies that every field of machine_config_t we persist makes it through
 * JSON encode/decode unchanged, and that bundle create/list/delete sequences
 * round-trip via the on-disk format. */

#include "launcher/vm_bundle.h"
#include "launcher/vm_config_json.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static void fail(const char *msg) {
    fprintf(stderr, "test_vm_config: %s\n", msg);
    exit(1);
}

#define CHECK(cond, msg) do { if (!(cond)) fail(msg); } while (0)

static void test_config_round_trip(void)
{
    char tmpl[] = "/tmp/be300_vmconfig_XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir, "mkdtemp failed");

    char json_path[1024];
    snprintf(json_path, sizeof json_path, "%s/config.json", dir);

    machine_config_t out;
    vm_config_apply_defaults(&out);
    out.trace = true;
    out.enable_ne2000 = true;
    out.enable_audio = true;
    out.restore = false;
    out.sdram_size = 32u * 1024u * 1024u;
    out.target_mhz = 200;
    out.scale = 2.5;
    out.fb_width = 480; out.fb_height = 640; out.fb_stride = 512;
    out.frame_visible = true;
    out.pcconnect_dock = BE300_PCC_DOCK_USB_SYNC;
    out.pcconnect_bridge = "tcp:127.0.0.1:5555";
    out.pcconnect_tee = "/tmp/pcc.log";
    out.serial0_bridge = "pty:auto";
    out.serial1_bridge = NULL;
    out.nand_path = "/path/to/nand.bin";
    out.cf_paths[0] = "/path/to/cf0.img";
    out.cf_paths[1] = NULL;
    out.cf_count = 1;
    out.net_mac_set = true;
    out.net_mac[0] = 0x10; out.net_mac[1] = 0x20; out.net_mac[2] = 0x30;
    out.net_mac[3] = 0xAA; out.net_mac[4] = 0xBB; out.net_mac[5] = 0xCC;

    int rc = vm_config_save(json_path, &out);
    CHECK(rc == 0, "vm_config_save returned non-zero");

    vm_bundle_t in;
    memset(&in, 0, sizeof in);
    snprintf(in.path, sizeof in.path, "%s", dir);
    rc = vm_config_load(json_path, &in);
    CHECK(rc == 0, "vm_config_load returned non-zero");

    CHECK(in.cfg.trace == true, "trace round-trip");
    CHECK(in.cfg.enable_ne2000 == true, "ne2000 round-trip");
    CHECK(in.cfg.enable_audio == true, "audio round-trip");
    CHECK(in.cfg.restore == false, "restore round-trip");
    CHECK(in.cfg.sdram_size == 32u * 1024u * 1024u, "sdram round-trip");
    CHECK(in.cfg.target_mhz == 200, "target_mhz round-trip");
    CHECK(in.cfg.scale == 2.5, "scale round-trip");
    CHECK(in.cfg.fb_width == 480, "fb_width round-trip");
    CHECK(in.cfg.fb_height == 640, "fb_height round-trip");
    CHECK(in.cfg.fb_stride == 512, "fb_stride round-trip");
    CHECK(in.cfg.frame_visible == true, "frame_visible round-trip");
    CHECK(in.cfg.pcconnect_dock == BE300_PCC_DOCK_USB_SYNC, "dock round-trip");

    CHECK(in.cfg.pcconnect_bridge && strcmp(in.cfg.pcconnect_bridge, "tcp:127.0.0.1:5555") == 0,
        "pcconnect_bridge round-trip");
    CHECK(in.cfg.pcconnect_tee && strcmp(in.cfg.pcconnect_tee, "/tmp/pcc.log") == 0,
        "pcconnect_tee round-trip");
    CHECK(in.cfg.serial0_bridge && strcmp(in.cfg.serial0_bridge, "pty:auto") == 0,
        "serial0 round-trip");
    CHECK(in.cfg.serial1_bridge == NULL, "serial1 stays null");
    CHECK(in.cfg.nand_path && strcmp(in.cfg.nand_path, "/path/to/nand.bin") == 0,
        "nand round-trip");
    CHECK(in.cfg.cf_count == 1, "cf_count round-trip");
    CHECK(in.cfg.cf_paths[0] && strcmp(in.cfg.cf_paths[0], "/path/to/cf0.img") == 0,
        "cf0 round-trip");
    CHECK(in.cfg.cf_paths[1] == NULL, "cf1 stays null");

    CHECK(in.cfg.net_mac_set == true, "net_mac_set round-trip");
    CHECK(in.cfg.net_mac[0] == 0x10 && in.cfg.net_mac[5] == 0xCC, "net_mac bytes");

    unlink(json_path);
    rmdir(dir);
    printf("test_config_round_trip OK\n");
}

static void test_mkdir_p(void)
{
    char base[] = "/tmp/be300_mkdir_XXXXXX";
    char *dir = mkdtemp(base);
    CHECK(dir, "mkdtemp");
    char deep[1024];
    snprintf(deep, sizeof deep, "%s/a/b/c/d", dir);
    int rc = vm_bundle_mkdir_p(deep);
    CHECK(rc == 0, "mkdir_p deep");
    struct stat st;
    CHECK(stat(deep, &st) == 0 && S_ISDIR(st.st_mode), "deep dir exists");
    /* clean up best-effort */
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    (void)system(cmd);
    printf("test_mkdir_p OK\n");
}

static void test_bundle_create_list_delete(void)
{
    /* Use a private XDG/HOME so we don't disturb the real user dir. */
    char fake_home[] = "/tmp/be300_homedir_XXXXXX";
    char *home = mkdtemp(fake_home);
    CHECK(home, "mkdtemp home");
    setenv("HOME", home, 1);
    setenv("XDG_DATA_HOME", home, 1);

    vm_bundle_t vm;
    int rc = vm_bundle_create("TestVM", &vm);
    CHECK(rc == 0, "vm_bundle_create");

    vm_bundle_list_t list;
    rc = vm_bundle_list(&list);
    CHECK(rc == 0, "vm_bundle_list");
    CHECK(list.count == 1, "list count == 1");
    CHECK(strcmp(list.items[0].name, "TestVM") == 0, "list item name");

    rc = vm_bundle_delete(&vm);
    CHECK(rc == 0, "vm_bundle_delete");

    vm_bundle_list_free(&list);

    rc = vm_bundle_list(&list);
    CHECK(rc == 0, "vm_bundle_list (post-delete)");
    CHECK(list.count == 0, "list empty after delete");
    vm_bundle_list_free(&list);

    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", home);
    (void)system(cmd);
    printf("test_bundle_create_list_delete OK\n");
}

int main(void)
{
    test_config_round_trip();
    test_mkdir_p();
    test_bundle_create_list_delete();
    printf("ALL OK\n");
    return 0;
}
