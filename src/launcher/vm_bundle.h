#ifndef BE300_LAUNCHER_VM_BUNDLE_H
#define BE300_LAUNCHER_VM_BUNDLE_H

#include <stddef.h>
#include "be300.h"

#ifndef BE300_VM_PATH_MAX
#define BE300_VM_PATH_MAX 1024
#endif

#define BE300_VM_NAME_MAX 128
#define BE300_VM_BRIDGE_MAX 256

#ifdef __cplusplus
extern "C" {
#endif

/* A vm_bundle_t owns its string storage. `cfg`'s const char* fields either
 * point into the owned buffers below or are NULL. Copy by value carries the
 * buffers along — but after a memcpy you must re-point cfg's pointers via
 * vm_bundle_rebind_strings() so they reference the destination's buffers, not
 * the source's. */
typedef struct vm_bundle {
    char path[BE300_VM_PATH_MAX];   /* absolute path to <name>.be300vm/ */
    char name[BE300_VM_NAME_MAX];   /* display name (basename without suffix) */
    machine_config_t cfg;

    /* Owned string storage backing cfg's const char* fields. */
    char nand_path_buf[BE300_VM_PATH_MAX];
    char cf_path_buf[BE300_MAX_CF_SLOTS][BE300_VM_PATH_MAX];
    char rom_path_buf[BE300_VM_PATH_MAX];
    char pcconnect_bridge_buf[BE300_VM_BRIDGE_MAX];
    char pcconnect_tee_buf[BE300_VM_PATH_MAX];
    char serial0_bridge_buf[BE300_VM_BRIDGE_MAX];
    char serial0_tee_buf[BE300_VM_PATH_MAX];
    char serial1_bridge_buf[BE300_VM_BRIDGE_MAX];
    char serial1_tee_buf[BE300_VM_PATH_MAX];
} vm_bundle_t;

typedef struct vm_bundle_list {
    vm_bundle_t *items;
    size_t       count;
    size_t       cap;
} vm_bundle_list_t;

/* Resolve the per-user VM root directory. Returns a pointer to a static
 * (or thread-local) buffer, valid until the next call. NULL on failure. */
const char *vm_bundle_root_dir(void);

/* Create the root directory if it does not already exist. Returns 0 on
 * success, -1 on error (errno set). */
int vm_bundle_ensure_root_dir(void);

/* mkdir -p semantics on a path. Returns 0 if the directory exists when the
 * call returns, -1 with errno set otherwise. */
int vm_bundle_mkdir_p(const char *path);

/* Enumerate *.be300vm bundles under the root dir. Caller frees with
 * vm_bundle_list_free(). Returns 0 on success, -1 on error. */
int vm_bundle_list(vm_bundle_list_t *out);
void vm_bundle_list_free(vm_bundle_list_t *list);

/* Create a fresh bundle directory and a default config.json in it. The
 * returned vm_bundle_t carries cfg defaults that match the CLI defaults
 * (sdram=16MB, target_mhz=166, etc.). Returns 0 on success, -1 on error. */
int vm_bundle_create(const char *name, vm_bundle_t *out);

/* Load an existing bundle by absolute path (e.g., Finder double-click).
 * Reads <path>/config.json. */
int vm_bundle_open_from_path(const char *bundle_path, vm_bundle_t *out);

/* Save the current cfg back to <bundle>/config.json. */
int vm_bundle_save(const vm_bundle_t *vm);

/* Re-point cfg's const char* fields at the bundle's owned string buffers.
 * Call this after memcpy-ing a vm_bundle_t. */
void vm_bundle_rebind_strings(vm_bundle_t *vm);

/* File copy NAND/CF source into the bundle. Updates cfg.nand_path or
 * cfg.cf_paths[slot] to point into the bundle's owned buffers. */
int vm_bundle_import_nand(vm_bundle_t *vm, const char *src_nand_path);
int vm_bundle_import_cf(vm_bundle_t *vm, unsigned slot, const char *src_cf_path);

/* Delete a bundle directory recursively. Returns 0 on success. */
int vm_bundle_delete(const vm_bundle_t *vm);

#ifdef __cplusplus
}
#endif

#endif /* BE300_LAUNCHER_VM_BUNDLE_H */
