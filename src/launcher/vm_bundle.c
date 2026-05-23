#include "vm_bundle.h"
#include "vm_config_json.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define BE300_PATH_SEP '\\'
#define BE300_PATH_SEP_STR "\\"
#else
#include <dirent.h>
#include <unistd.h>
#define BE300_PATH_SEP '/'
#define BE300_PATH_SEP_STR "/"
#endif

#define BE300_VM_SUFFIX ".be300vm"
#define BE300_VM_SUFFIX_LEN 8

static int copy_file(const char *src, const char *dst)
{
    FILE *fi = fopen(src, "rb");
    if (!fi)
        return -1;
    FILE *fo = fopen(dst, "wb");
    if (!fo) {
        int e = errno;
        fclose(fi);
        errno = e;
        return -1;
    }
    char buf[64 * 1024];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof buf, fi)) > 0) {
        if (fwrite(buf, 1, n, fo) != n) { rc = -1; break; }
    }
    if (ferror(fi)) rc = -1;
    if (fclose(fo) != 0) rc = -1;
    fclose(fi);
    return rc;
}

static int path_exists_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

static int single_mkdir(const char *path)
{
#ifdef _WIN32
    if (_mkdir(path) == 0) return 0;
#else
    if (mkdir(path, 0755) == 0) return 0;
#endif
    if (errno == EEXIST && path_exists_dir(path)) return 0;
    return -1;
}

int vm_bundle_mkdir_p(const char *path)
{
    if (!path || !*path) { errno = EINVAL; return -1; }

    char buf[BE300_VM_PATH_MAX];
    size_t n = strlen(path);
    if (n >= sizeof buf) { errno = ENAMETOOLONG; return -1; }
    memcpy(buf, path, n + 1);

    /* Skip the leading separator (POSIX absolute path) or drive prefix
     * (Windows). On Windows, mkdir("C:") fails, so we have to step past
     * "C:\" before walking. */
    size_t start = 1;
#ifdef _WIN32
    if (n >= 3 && buf[1] == ':' &&
        (buf[2] == '\\' || buf[2] == '/')) {
        start = 3;
    }
#endif

    /* Walk components left-to-right, creating each parent. */
    for (size_t i = start; i < n; i++) {
        if (buf[i] == '/' || buf[i] == BE300_PATH_SEP) {
            char saved = buf[i];
            buf[i] = '\0';
            if (single_mkdir(buf) != 0) return -1;
            buf[i] = saved;
        }
    }
    return single_mkdir(buf);
}

const char *vm_bundle_root_dir(void)
{
    static char root[BE300_VM_PATH_MAX];

#ifdef _WIN32
    const char *base = getenv("APPDATA");
    if (!base || !*base) return NULL;
    snprintf(root, sizeof root, "%s\\BE300\\VMs", base);
#elif defined(__APPLE__)
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;
    snprintf(root, sizeof root, "%s/Library/Application Support/BE300/VMs", home);
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        snprintf(root, sizeof root, "%s/BE300/VMs", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) return NULL;
        snprintf(root, sizeof root, "%s/.local/share/BE300/VMs", home);
    }
#endif
    return root;
}

int vm_bundle_ensure_root_dir(void)
{
    const char *root = vm_bundle_root_dir();
    if (!root) { errno = ENOENT; return -1; }
    return vm_bundle_mkdir_p(root);
}

static int list_grow(vm_bundle_list_t *list)
{
    size_t new_cap = list->cap ? list->cap * 2 : 8;
    vm_bundle_t *items = realloc(list->items, new_cap * sizeof(*items));
    if (!items) return -1;
    list->items = items;
    list->cap = new_cap;
    return 0;
}

void vm_bundle_list_free(vm_bundle_list_t *list)
{
    if (!list) return;
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

void vm_bundle_rebind_strings(vm_bundle_t *vm)
{
    if (!vm) return;
    vm->cfg.nand_path        = vm->nand_path_buf[0]        ? vm->nand_path_buf        : NULL;
    vm->cfg.rom_path         = vm->rom_path_buf[0]         ? vm->rom_path_buf         : NULL;
    for (unsigned s = 0; s < BE300_MAX_CF_SLOTS; s++) {
        vm->cfg.cf_paths[s] = vm->cf_path_buf[s][0] ? vm->cf_path_buf[s] : NULL;
    }
    vm->cfg.pcconnect_bridge = vm->pcconnect_bridge_buf[0] ? vm->pcconnect_bridge_buf : NULL;
    vm->cfg.pcconnect_tee    = vm->pcconnect_tee_buf[0]    ? vm->pcconnect_tee_buf    : NULL;
    vm->cfg.serial0_bridge   = vm->serial0_bridge_buf[0]   ? vm->serial0_bridge_buf   : NULL;
    vm->cfg.serial0_tee      = vm->serial0_tee_buf[0]      ? vm->serial0_tee_buf      : NULL;
    vm->cfg.serial1_bridge   = vm->serial1_bridge_buf[0]   ? vm->serial1_bridge_buf   : NULL;
    vm->cfg.serial1_tee      = vm->serial1_tee_buf[0]      ? vm->serial1_tee_buf      : NULL;
}

static int load_bundle_at(const char *bundle_path, vm_bundle_t *out)
{
    memset(out, 0, sizeof *out);
    size_t n = strlen(bundle_path);
    if (n >= sizeof out->path) { errno = ENAMETOOLONG; return -1; }
    memcpy(out->path, bundle_path, n + 1);
    /* Strip trailing slash if any. */
    if (n > 0 && (out->path[n - 1] == '/' || out->path[n - 1] == BE300_PATH_SEP))
        out->path[--n] = '\0';

    /* Derive name = basename without .be300vm suffix. */
    const char *base = out->path;
    for (size_t i = n; i-- > 0; ) {
        if (out->path[i] == '/' || out->path[i] == BE300_PATH_SEP) {
            base = out->path + i + 1;
            break;
        }
    }
    size_t bn = strlen(base);
    if (bn > BE300_VM_SUFFIX_LEN &&
        strcmp(base + bn - BE300_VM_SUFFIX_LEN, BE300_VM_SUFFIX) == 0) {
        size_t nm = bn - BE300_VM_SUFFIX_LEN;
        if (nm >= sizeof out->name) nm = sizeof out->name - 1;
        memcpy(out->name, base, nm);
        out->name[nm] = '\0';
    } else {
        size_t nm = bn;
        if (nm >= sizeof out->name) nm = sizeof out->name - 1;
        memcpy(out->name, base, nm);
        out->name[nm] = '\0';
    }

    vm_config_apply_defaults(&out->cfg);

    char config_path[BE300_VM_PATH_MAX];
    snprintf(config_path, sizeof config_path, "%s%cconfig.json",
        out->path, BE300_PATH_SEP);
    if (vm_config_load(config_path, out) != 0) return -1;
    vm_bundle_rebind_strings(out);
    return 0;
}

int vm_bundle_open_from_path(const char *bundle_path, vm_bundle_t *out)
{
    if (!bundle_path || !out) { errno = EINVAL; return -1; }
    return load_bundle_at(bundle_path, out);
}

int vm_bundle_list(vm_bundle_list_t *out)
{
    if (!out) { errno = EINVAL; return -1; }
    memset(out, 0, sizeof *out);

    const char *root = vm_bundle_root_dir();
    if (!root) { errno = ENOENT; return -1; }
    if (vm_bundle_ensure_root_dir() != 0) return -1;

#ifdef _WIN32
    char pattern[BE300_VM_PATH_MAX];
    snprintf(pattern, sizeof pattern, "%s\\*", root);
    WIN32_FIND_DATAA find;
    HANDLE h = FindFirstFileA(pattern, &find);
    if (h == INVALID_HANDLE_VALUE) {
        /* Empty root is fine. */
        return 0;
    }
    do {
        if (!(find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const char *name = find.cFileName;
        size_t nl = strlen(name);
        if (nl <= BE300_VM_SUFFIX_LEN) continue;
        if (strcmp(name + nl - BE300_VM_SUFFIX_LEN, BE300_VM_SUFFIX) != 0)
            continue;
        if (out->count == out->cap && list_grow(out) != 0) {
            FindClose(h);
            vm_bundle_list_free(out);
            return -1;
        }
        char bundle_path[BE300_VM_PATH_MAX];
        snprintf(bundle_path, sizeof bundle_path, "%s\\%s", root, name);
        if (load_bundle_at(bundle_path, &out->items[out->count]) == 0)
            out->count++;
    } while (FindNextFileA(h, &find));
    FindClose(h);
#else
    DIR *d = opendir(root);
    if (!d) {
        if (errno == ENOENT) return 0; /* root just doesn't exist yet */
        return -1;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (name[0] == '.') continue;
        size_t nl = strlen(name);
        if (nl <= BE300_VM_SUFFIX_LEN) continue;
        if (strcmp(name + nl - BE300_VM_SUFFIX_LEN, BE300_VM_SUFFIX) != 0)
            continue;
        char bundle_path[BE300_VM_PATH_MAX];
        snprintf(bundle_path, sizeof bundle_path, "%s/%s", root, name);
        struct stat st;
        if (stat(bundle_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (out->count == out->cap && list_grow(out) != 0) {
            closedir(d);
            vm_bundle_list_free(out);
            return -1;
        }
        if (load_bundle_at(bundle_path, &out->items[out->count]) == 0)
            out->count++;
    }
    closedir(d);
#endif
    return 0;
}

static int sanitize_name(const char *src, char *dst, size_t dst_cap)
{
    if (!src || !*src || !dst || dst_cap < 2) { errno = EINVAL; return -1; }
    size_t out = 0;
    for (const char *p = src; *p && out + 1 < dst_cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
            c == 0x7f)
            continue;
        dst[out++] = (char)c;
    }
    dst[out] = '\0';
    if (out == 0) { errno = EINVAL; return -1; }
    return 0;
}

int vm_bundle_create(const char *name, vm_bundle_t *out)
{
    if (!name || !out) { errno = EINVAL; return -1; }
    if (vm_bundle_ensure_root_dir() != 0) return -1;

    char clean[BE300_VM_NAME_MAX];
    if (sanitize_name(name, clean, sizeof clean) != 0) return -1;

    const char *root = vm_bundle_root_dir();
    if (!root) { errno = ENOENT; return -1; }

    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", clean);
    snprintf(out->path, sizeof out->path, "%s%c%s%s",
        root, BE300_PATH_SEP, clean, BE300_VM_SUFFIX);

    if (path_exists_dir(out->path)) { errno = EEXIST; return -1; }
    if (vm_bundle_mkdir_p(out->path) != 0) return -1;

    char screenshots_dir[BE300_VM_PATH_MAX];
    snprintf(screenshots_dir, sizeof screenshots_dir, "%s%cscreenshots",
        out->path, BE300_PATH_SEP);
    (void)vm_bundle_mkdir_p(screenshots_dir);

    vm_config_apply_defaults(&out->cfg);
    vm_bundle_rebind_strings(out);
    return vm_bundle_save(out);
}

int vm_bundle_save(const vm_bundle_t *vm)
{
    if (!vm) { errno = EINVAL; return -1; }
    char config_path[BE300_VM_PATH_MAX];
    snprintf(config_path, sizeof config_path, "%s%cconfig.json",
        vm->path, BE300_PATH_SEP);
    return vm_config_save(config_path, &vm->cfg);
}

int vm_bundle_import_nand(vm_bundle_t *vm, const char *src_nand_path)
{
    if (!vm || !src_nand_path) { errno = EINVAL; return -1; }
    snprintf(vm->nand_path_buf, sizeof vm->nand_path_buf, "%s%cnand.bin",
        vm->path, BE300_PATH_SEP);
    if (copy_file(src_nand_path, vm->nand_path_buf) != 0) {
        vm->nand_path_buf[0] = '\0';
        vm_bundle_rebind_strings(vm);
        return -1;
    }
    vm_bundle_rebind_strings(vm);
    return vm_bundle_save(vm);
}

int vm_bundle_import_cf(vm_bundle_t *vm, unsigned slot, const char *src_cf_path)
{
    if (!vm || !src_cf_path || slot >= BE300_MAX_CF_SLOTS) {
        errno = EINVAL; return -1;
    }
    snprintf(vm->cf_path_buf[slot], sizeof vm->cf_path_buf[slot],
        "%s%ccf%u.bin", vm->path, BE300_PATH_SEP, slot);
    if (copy_file(src_cf_path, vm->cf_path_buf[slot]) != 0) {
        vm->cf_path_buf[slot][0] = '\0';
        vm_bundle_rebind_strings(vm);
        return -1;
    }
    if (slot >= vm->cfg.cf_count) vm->cfg.cf_count = slot + 1;
    vm_bundle_rebind_strings(vm);
    return vm_bundle_save(vm);
}

#ifdef _WIN32
static int rm_rf(const char *path)
{
    char pattern[BE300_VM_PATH_MAX];
    snprintf(pattern, sizeof pattern, "%s\\*", path);
    WIN32_FIND_DATAA find;
    HANDLE h = FindFirstFileA(pattern, &find);
    if (h == INVALID_HANDLE_VALUE) {
        return RemoveDirectoryA(path) ? 0 : -1;
    }
    int rc = 0;
    do {
        const char *n = find.cFileName;
        if (strcmp(n, ".") == 0 || strcmp(n, "..") == 0) continue;
        char child[BE300_VM_PATH_MAX];
        snprintf(child, sizeof child, "%s\\%s", path, n);
        if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (rm_rf(child) != 0) rc = -1;
        } else {
            if (!DeleteFileA(child)) rc = -1;
        }
    } while (FindNextFileA(h, &find));
    FindClose(h);
    if (!RemoveDirectoryA(path)) rc = -1;
    return rc;
}
#else
static int rm_rf(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return rmdir(path);
    int rc = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char child[BE300_VM_PATH_MAX];
        snprintf(child, sizeof child, "%s/%s", path, de->d_name);
        struct stat st;
        if (lstat(child, &st) != 0) { rc = -1; continue; }
        if (S_ISDIR(st.st_mode)) {
            if (rm_rf(child) != 0) rc = -1;
        } else {
            if (unlink(child) != 0) rc = -1;
        }
    }
    closedir(d);
    if (rmdir(path) != 0) rc = -1;
    return rc;
}
#endif

int vm_bundle_delete(const vm_bundle_t *vm)
{
    if (!vm || !vm->path[0]) { errno = EINVAL; return -1; }
    /* Refuse to recurse into anything that doesn't carry the .be300vm
     * suffix — defensive, since the launcher will only ever call this on
     * vm_bundle_t values that came from create/list/open. */
    size_t n = strlen(vm->path);
    if (n <= BE300_VM_SUFFIX_LEN ||
        strcmp(vm->path + n - BE300_VM_SUFFIX_LEN, BE300_VM_SUFFIX) != 0) {
        errno = EINVAL; return -1;
    }
    return rm_rf(vm->path);
}

static void rebase_owned_path(char *buf, size_t cap,
                              const char *old_prefix, const char *new_prefix)
{
    if (!buf || !*buf) return;
    size_t pn = strlen(old_prefix);
    if (strncmp(buf, old_prefix, pn) != 0) return;
    char tmp[BE300_VM_PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s%s", new_prefix, buf + pn);
    snprintf(buf, cap, "%s", tmp);
}

int vm_bundle_rename(vm_bundle_t *vm, const char *new_name)
{
    if (!vm || !new_name) { errno = EINVAL; return -1; }

    char clean[BE300_VM_NAME_MAX];
    if (sanitize_name(new_name, clean, sizeof clean) != 0) return -1;
    if (strcmp(clean, vm->name) == 0) return 0; /* no-op */

    const char *root = vm_bundle_root_dir();
    if (!root) { errno = ENOENT; return -1; }

    char new_path[BE300_VM_PATH_MAX];
    snprintf(new_path, sizeof new_path, "%s%c%s%s",
        root, BE300_PATH_SEP, clean, BE300_VM_SUFFIX);
    if (path_exists_dir(new_path)) { errno = EEXIST; return -1; }

    /* rename(2) is atomic on the same volume on every supported host. */
    if (rename(vm->path, new_path) != 0) return -1;

    char old_path[BE300_VM_PATH_MAX];
    snprintf(old_path, sizeof old_path, "%s", vm->path);

    snprintf(vm->name, sizeof vm->name, "%s", clean);
    snprintf(vm->path, sizeof vm->path, "%s", new_path);

    rebase_owned_path(vm->nand_path_buf, sizeof vm->nand_path_buf, old_path, new_path);
    rebase_owned_path(vm->rom_path_buf,  sizeof vm->rom_path_buf,  old_path, new_path);
    for (unsigned s = 0; s < BE300_MAX_CF_SLOTS; s++) {
        rebase_owned_path(vm->cf_path_buf[s], sizeof vm->cf_path_buf[s],
                          old_path, new_path);
    }
    /* Bridges/tees may live outside the bundle; only rewrite if they
     * actually point inside the old bundle dir. */
    rebase_owned_path(vm->pcconnect_tee_buf, sizeof vm->pcconnect_tee_buf, old_path, new_path);
    rebase_owned_path(vm->serial0_tee_buf,   sizeof vm->serial0_tee_buf,   old_path, new_path);
    rebase_owned_path(vm->serial1_tee_buf,   sizeof vm->serial1_tee_buf,   old_path, new_path);

    vm_bundle_rebind_strings(vm);
    return 0;
}
