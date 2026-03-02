#pragma once

#include "machine.h"

int  ui_init(machine_t *m);
void ui_update(machine_t *m);
void ui_destroy(machine_t *m);
bool ui_should_quit(machine_t *m);
