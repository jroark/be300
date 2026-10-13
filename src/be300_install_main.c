/*
 * be300-install: install Casio .cbea application packages into a BE-300
 * NAND image without the Windows Setup.exe / PC Connect / ActiveSync chain.
 * See docs/INSTALLING_APPS.md.
 */

#include "cbea_install.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define LOG_SIZE (64u * 1024u)

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [--output OUT | --in-place] [--force] <nand.bin> <package>...\n"
        "\n"
        "Installs Casio .cbea application packages into a BE-300 NAND image.\n"
        "<package> is a .cbea file, a Setup.ini, or a package directory (one\n"
        "with Setup.ini, or with MAIN/ and CF/ variants; MAIN/ is used).\n"
        "\n"
        "  --output OUT   write the result to OUT (default: <nand>.installed.bin)\n"
        "  --in-place     modify <nand.bin> directly\n"
        "  --force        allow --output to overwrite an existing file\n"
        "\n"
        "Do not run this on an image a running be300 is using: the emulator\n"
        "rewrites the NAND file when it exits.\n",
        argv0);
}

static bool is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static uint8_t *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf = NULL;
    long sz;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (sz = ftell(f)) >= 0 &&
        fseek(f, 0, SEEK_SET) == 0) {
        buf = malloc(sz ? (size_t)sz : 1);
        if (buf && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
            free(buf);
            buf = NULL;
        }
        *len = (size_t)sz;
    }
    fclose(f);
    return buf;
}

static bool ends_with_ci(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);

    if (m > n)
        return false;
    for (size_t i = 0; i < m; i++) {
        char a = s[n - m + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z')
            b = (char)(b + 32);
        if (a != b)
            return false;
    }
    return true;
}

/* Setup.ini: "[APPNAME] ... file = <name>.cbea" (WMP 1.1 MAIN/Setup.ini). */
static bool setup_ini_cbea(const char *ini, char *out, size_t cap)
{
    FILE *f = fopen(ini, "rb");
    char line[512];
    bool found = false;

    if (!f)
        return false;
    while (!found && fgets(line, sizeof(line), f)) {
        char *p = line, *eq, *v, *e;
        while (*p == ' ' || *p == '\t')
            p++;
        if (strncmp(p, "file", 4) != 0 && strncmp(p, "FILE", 4) != 0)
            continue;
        eq = strchr(p, '=');
        if (!eq)
            continue;
        v = eq + 1;
        while (*v == ' ' || *v == '\t')
            v++;
        e = v + strlen(v);
        while (e > v && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' '))
            *--e = '\0';
        if (*v) {
            const char *slash = strrchr(ini, '/');
            const char *bslash = strrchr(ini, '\\');
            if (bslash && (!slash || bslash > slash))
                slash = bslash;
            int w = slash ? snprintf(out, cap, "%.*s/%s", (int)(slash - ini), ini, v)
                          : snprintf(out, cap, "%s", v);
            found = w > 0 && (size_t)w < cap;
        }
    }
    fclose(f);
    return found;
}

static bool single_cbea_in_dir(const char *dir, char *out, size_t cap)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    int count = 0;

    if (!d)
        return false;
    while ((de = readdir(d)) != NULL) {
        if (ends_with_ci(de->d_name, ".cbea")) {
            snprintf(out, cap, "%s/%s", dir, de->d_name);
            count++;
        }
    }
    closedir(d);
    return count == 1;
}

static bool resolve_package(const char *arg, char *out, size_t cap)
{
    char ini[4096];

    if (is_file(arg)) {
        if (ends_with_ci(arg, ".ini"))
            return setup_ini_cbea(arg, out, cap);
        snprintf(out, cap, "%s", arg);
        return true;
    }
    if (!is_dir(arg))
        return false;
    snprintf(ini, sizeof(ini), "%s/Setup.ini", arg);
    if (is_file(ini))
        return setup_ini_cbea(ini, out, cap);
    snprintf(ini, sizeof(ini), "%s/MAIN/Setup.ini", arg);
    if (is_file(ini))
        return setup_ini_cbea(ini, out, cap);
    return single_cbea_in_dir(arg, out, cap);
}

static int write_all(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    size_t w;

    if (!f)
        return -1;
    w = fwrite(data, 1, len, f);
    if (fclose(f) != 0 || w != len)
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *output = NULL, *nand_path = NULL;
    char default_out[4096];
    bool in_place = false, force = false;
    int first_pkg = 0;
    uint8_t *nand;
    size_t nand_len = 0;
    char *log;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (strcmp(argv[i], "--in-place") == 0) {
            in_place = true;
        } else if (strcmp(argv[i], "--force") == 0) {
            force = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] == '-') {
            fprintf(stderr, "error: unknown option %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        } else {
            nand_path = argv[i];
            first_pkg = i + 1;
            break;
        }
    }
    if (!nand_path || first_pkg >= argc || (in_place && output)) {
        usage(argv[0]);
        return 2;
    }
    if (in_place) {
        output = nand_path;
    } else if (!output) {
        size_t n = strlen(nand_path);
        if (ends_with_ci(nand_path, ".bin"))
            n -= 4;
        snprintf(default_out, sizeof(default_out), "%.*s.installed.bin", (int)n, nand_path);
        output = default_out;
    }
    if (!in_place && !force && is_file(output)) {
        fprintf(stderr, "error: refusing to overwrite existing output %s (use --force)\n", output);
        return 1;
    }

    nand = read_all(nand_path, &nand_len);
    if (!nand) {
        fprintf(stderr, "error: cannot read NAND image %s: %s\n", nand_path, strerror(errno));
        return 1;
    }
    log = malloc(LOG_SIZE);
    if (!log) {
        free(nand);
        return 1;
    }

    for (int i = first_pkg; i < argc; i++) {
        char pkg_path[4096];
        uint8_t *pkg;
        size_t pkg_len = 0;
        int rc;

        if (!resolve_package(argv[i], pkg_path, sizeof(pkg_path))) {
            fprintf(stderr, "error: cannot find a .cbea package in %s\n", argv[i]);
            goto fail;
        }
        pkg = read_all(pkg_path, &pkg_len);
        if (!pkg) {
            fprintf(stderr, "error: cannot read %s: %s\n", pkg_path, strerror(errno));
            goto fail;
        }
        printf("%s:\n", pkg_path);
        rc = cbea_install(nand, nand_len, pkg, pkg_len, log, LOG_SIZE);
        free(pkg);
        fputs(log, rc == 0 ? stdout : stderr);
        if (rc != 0) {
            fprintf(stderr, "error: install of %s failed; no output written\n", pkg_path);
            goto fail;
        }
    }

    if (write_all(output, nand, nand_len) != 0) {
        fprintf(stderr, "error: cannot write %s: %s\n", output, strerror(errno));
        goto fail;
    }
    printf("Wrote %s\n", output);
    free(log);
    free(nand);
    return 0;

fail:
    free(log);
    free(nand);
    return 1;
}
