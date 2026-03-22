/*
 *  stubs.c — Stub functions for GXemul subsystems not used by be300.
 *
 *  Provides minimal implementations for: debugger, diskimage,
 *  PROM emulation, bootblock, file loader, and other unused subsystems.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "cpu.h"
#include "device.h"
#include "emul.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"
#include "settings.h"

/*  Globals referenced by GXemul core  */
bool emul_executing = false;
bool emul_shutdown = false;
bool single_step = false;
bool debugger_enter_at_end_of_run = false;
bool debugmsg_executing_noninteractively = true;
bool about_to_enter_single_step = false;

int extra_argc = 0;
char **extra_argv = NULL;

uint64_t single_step_breakpoint = 0;
size_t dyntrans_cache_size = DEFAULT_DYNTRANS_CACHE_SIZE;

/*  console/settings globals — initialized by be300_create  */
const char *progname = "be300";
struct settings *global_settings = NULL;

/* Must be called before console_init() */
void be300_init_global_settings(void) {
	if (!global_settings)
		global_settings = settings_new();
}

/*  debugger stubs  */
void debugger_reset(void) { }
void debugger_init(struct emul *e) { (void)e; }
void debugger_activate(int sig) { (void)sig; }
void debugger_execute_cmd(const char *cmd, int len) { (void)cmd; (void)len; }

/*  diskimage stubs  */
void diskimage_dump_info(struct machine *m) { (void)m; }
int diskimage_bootdev(struct machine *m, int *type) {
	(void)m; (void)type;
	return -1;
}

/*  bootblock stub  */
int load_bootblock(struct machine *m, struct cpu *cpu,
    int *n_loadp, char ***load_namesp) {
	(void)m; (void)cpu; (void)n_loadp; (void)load_namesp;
	return 0;
}

/*  PROM emulation stubs  */
int decstation_prom_emul(struct cpu *cpu) { (void)cpu; return 0; }
int playstation2_sifbios_emul(struct cpu *cpu) { (void)cpu; return 0; }
int yamon_emul(struct cpu *cpu) { (void)cpu; return 0; }
int arcbios_emul(struct cpu *cpu) { (void)cpu; return 0; }

/*  file loader stubs  */
static int n_executables_loaded = 0;
int file_n_executables_loaded(void) { return n_executables_loaded; }
void file_load(struct machine *machine, struct memory *mem,
    char *filename, uint64_t *entrypointp,
    int arch, uint64_t *gpp, int *byte_order, uint64_t *tocp) {
	(void)machine; (void)mem; (void)filename; (void)arch;
	(void)gpp; (void)byte_order; (void)tocp;
	if (entrypointp) *entrypointp = 0;
	n_executables_loaded++;
}

/*  symbol demangling stub  */
char *symbol_demangle_cplusplus(const char *name) {
	(void)name;
	return NULL;
}

/*  autodev — register only the devices we need  */
extern int devinit_ns16550(struct devinit *);
void autodev_init(void) {
	device_register("ns16550", devinit_ns16550);
}

/*  emul_new / emul_add_machine — simplified from GXemul emul.c  */
struct emul *emul_new(char *name)
{
	struct emul *e;
	CHECK_ALLOCATION(e = (struct emul *) malloc(sizeof(struct emul)));
	memset(e, 0, sizeof(struct emul));
	e->settings = settings_new();
	e->n_machines = 0;
	e->next_serial_nr = 1;
	if (name != NULL)
		e->name = strdup(name);
	return e;
}

struct machine *emul_add_machine(struct emul *e, char *name)
{
	struct machine *m = machine_new(name, e, e->n_machines);
	m->serial_nr = (e->next_serial_nr++);
	int i = e->n_machines++;
	CHECK_ALLOCATION(e->machines = (struct machine **) realloc(e->machines,
	    sizeof(struct machine *) * e->n_machines));
	e->machines[i] = m;
	return m;
}

/*  automachine — register only the machine types we need  */
extern void machine_register_hpcmips(void);
void automachine_init(void) {
	machine_register_hpcmips();
}
