#pragma once

#include "be300.h"

/* Create, run and destroy a machine for `cfg`, repeating with a cold boot
 * while its window asks for a restart (and installing a requested app
 * package in between). On return, cfg->frame_visible and cfg->scale hold
 * the window's final bezel and zoom so callers can persist them.
 * Returns 0, or 1 when the machine could not be created. */
int be300_session_run(machine_config_t *cfg);
