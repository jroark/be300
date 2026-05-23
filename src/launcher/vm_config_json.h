#ifndef BE300_LAUNCHER_VM_CONFIG_JSON_H
#define BE300_LAUNCHER_VM_CONFIG_JSON_H

#include "vm_bundle.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BE300_VM_CONFIG_SCHEMA 1

/* Populate vm->cfg (and the backing string buffers) from json_path.
 * Bundle path and name must already be set on vm; this only reads cfg fields.
 * Returns 0 on success, -1 on parse/IO error. */
int vm_config_load(const char *json_path, vm_bundle_t *vm);

/* Serialize vm->cfg to json_path with schema version BE300_VM_CONFIG_SCHEMA.
 * Returns 0 on success, -1 on error. */
int vm_config_save(const char *json_path, const machine_config_t *cfg);

/* Apply the same defaults main.c uses to a freshly-zeroed machine_config_t. */
void vm_config_apply_defaults(machine_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* BE300_LAUNCHER_VM_CONFIG_JSON_H */
