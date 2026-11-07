/*
 *  be300_session.c — run one VM from the CLI or the launcher, including the
 *  cold-boot restarts its window asks for (Device > Restart, and app
 *  installs from Install App / a dropped .cbea).
 *
 *  A restart is a full machine teardown and rebuild, exactly like pulling
 *  the battery: the NAND image is written back by be300_destroy and the
 *  next machine boots from the ROM reset vector. Nothing is carried over
 *  in guest RAM. An app install is applied to the in-memory NAND between
 *  the two, with the same offline installer as be300-install
 *  (src/cbea_install.c, docs/INSTALLING_APPS.md).
 */
#include "be300_session.h"
#include "be300.h"
#include "cbea_install.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SESSION_PATH_MAX 4096
#define INSTALL_LOG_SIZE (16u * 1024u)

static uint8_t *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf = NULL;
    long sz;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (sz = ftell(f)) > 0 &&
        fseek(f, 0, SEEK_SET) == 0) {
        buf = malloc((size_t)sz);
        if (buf && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
            free(buf);
            buf = NULL;
        }
        *len_out = (size_t)sz;
    }
    fclose(f);
    return buf;
}

/* Install into the stopped machine's NAND; be300_destroy saves it. */
static void install_package(machine_t *m, const char *path)
{
    char *log = calloc(1, INSTALL_LOG_SIZE);
    size_t pkg_len = 0, image_len;
    uint8_t *pkg = read_file(path, &pkg_len);

    if (!pkg || !m->nand_data) {
        fprintf(stderr, "[BE300] Install: cannot read %s\n", path);
        free(pkg);
        free(log);
        return;
    }
    image_len = m->nand_size;
    if (m->nand_file_size > 0 && m->nand_file_size < image_len)
        image_len = m->nand_file_size;
    if (cbea_install(m->nand_data, image_len, pkg, pkg_len, log,
        log ? INSTALL_LOG_SIZE : 0) == 0) {
        m->nand.dirty = true;
        fprintf(stderr, "[BE300] Installed %s\n", path);
    } else {
        fprintf(stderr, "[BE300] Install of %s failed\n", path);
    }
    if (log && log[0])
        fprintf(stderr, "%s%s", log, log[strlen(log) - 1] == '\n' ? "" : "\n");
    free(pkg);
    free(log);
}

int be300_session_run(machine_config_t *cfg)
{
    char package[SESSION_PATH_MAX];
    bool bezel;
    double zoom;

    ui_session_begin();
    for (;;) {
        machine_t *m = be300_create(cfg);
        bool restart;

        if (!m) {
            fprintf(stderr, "Failed to create machine\n");
            return 1;
        }
        be300_run(m);
        restart = ui_take_restart_request(package, sizeof(package));
        if (restart && package[0])
            install_package(m, package);
        be300_destroy(m);
        if (!restart)
            break;
        fprintf(stderr, "[BE300] Restarting with a cold boot\n");
    }

    if (ui_window_settings(&bezel, &zoom)) {
        cfg->frame_visible = bezel;
        cfg->scale = zoom;
    }
    return 0;
}
