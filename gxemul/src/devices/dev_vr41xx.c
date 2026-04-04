/*
 *  Copyright (C) 2004-2009  Anders Gavare.  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright  
 *     notice, this list of conditions and the following disclaimer in the 
 *     documentation and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 *  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE   
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 *  OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 *  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 *  OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 *  SUCH DAMAGE.
 *   
 *
 *  COMMENT: VR41xx (VR4122 and VR4131) misc functions
 *
 *  This is just a big hack.
 *
 *  TODO: Implement more functionality some day.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "cpu.h"
#include "device.h"
#include "devices.h"
#include "interrupt.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"
#include "timer.h"

#include "thirdparty/bcureg.h"
#include "thirdparty/vripreg.h"
#include "thirdparty/vrkiureg.h"
#include "thirdparty/vr_rtcreg.h"

#include "hw/rtc.h"
#include "hw/cmu.h"
#include "hw/pmu.h"
#include "cop0.h"
#include "wince_boot.h"


/*  #define debug fatal  */

#define	DEV_VR41XX_TICKSHIFT		14

#define	DEV_VR41XX_LENGTH		0x800	/*  up to (not including) SIU at 0x800  */
struct vr41xx_data {
	struct interrupt cpu_irq;		/*  Connected to MIPS irq 2  */
	int		cpumodel;		/*  Model nr, e.g. 4121  */

	/*  KIU:  */
	int		kiu_console_handle;
	uint32_t	kiu_offset;
	struct interrupt kiu_irq;
	int		kiu_int_assert;
	int		old_kiu_int_assert;

	int		d0, d1, d2, d3, d4, d5;
	int		dont_clear_next;
	int		escape_state;

	/*  Timer:  */
	int		pending_timer_interrupts;
	int		replay_etimer_suppressed;
	int		rtcl1_irq_asserted;
	struct interrupt timer_irq;
	struct interrupt rtcl1_irq;
	struct timer	*timer;

	/*  See icureg.h in NetBSD for more info.  */
	uint16_t	sysint1;
	uint16_t	msysint1;
	uint16_t	giuint;
	uint16_t	giumask;
	uint16_t	sysint2;
	uint16_t	msysint2;
	uint16_t	giu_regs[16];
	struct interrupt giu_irq;

	/*  VR4131 RTC (elapsed time counter with read-assist):  */
	rtc_state_t	rtc;
	cmu_state_t	cmu;
	pmu_state_t	pmu;
	uint8_t		dmaau_regs[0x20];
	uint8_t		dcu_regs[0x20];
	uint8_t		sdramu_regs[0x10];
};

struct vr41xx_diag_binding {
	struct machine *machine;
	struct vr41xx_data *data;
	struct vr41xx_diag_binding *next;
};

static struct vr41xx_diag_binding *g_vr41xx_diag_bindings = NULL;

static void vr41xx_diag_register(struct machine *machine,
	struct vr41xx_data *data)
{
	struct vr41xx_diag_binding *binding;

	for (binding = g_vr41xx_diag_bindings; binding != NULL;
	    binding = binding->next) {
		if (binding->machine == machine) {
			binding->data = data;
			return;
		}
	}

	CHECK_ALLOCATION(binding = (struct vr41xx_diag_binding *)malloc(
	    sizeof(struct vr41xx_diag_binding)));
	binding->machine = machine;
	binding->data = data;
	binding->next = g_vr41xx_diag_bindings;
	g_vr41xx_diag_bindings = binding;
}

bool vr41xx_diag_get_state(struct machine *machine,
	struct vr41xx_diag_state *out)
{
	struct vr41xx_diag_binding *binding;
	struct vr41xx_data *d;

	if (out != NULL)
		memset(out, 0, sizeof(*out));
	if (machine == NULL || out == NULL)
		return false;

	for (binding = g_vr41xx_diag_bindings; binding != NULL;
	    binding = binding->next) {
		if (binding->machine == machine)
			break;
	}
	if (binding == NULL || binding->data == NULL)
		return false;

	d = binding->data;
	out->sysint1 = d->sysint1;
	out->msysint1 = d->msysint1;
	out->giuint = d->giuint;
	out->giumask = d->giumask;
	out->giuintenl = d->giu_regs[(0x14c - 0x140) / 2];
	out->sysint2 = d->sysint2;
	out->msysint2 = d->msysint2;
	out->pending_timer_interrupts = (uint32_t)d->pending_timer_interrupts;
	out->pmucntreg = d->pmu.pmucntreg;
	return true;
}

static uint64_t vr41xx_latch_read(const uint8_t *regs, uint32_t off,
	unsigned len)
{
	uint64_t value = 0;
	unsigned i;

	for (i = 0; i < len; i++)
		value |= (uint64_t)regs[off + i] << (i * 8);

	return value;
}

static void vr41xx_latch_write(uint8_t *regs, uint32_t off, unsigned len,
	uint64_t value)
{
	unsigned i;

	for (i = 0; i < len; i++)
		regs[off + i] = (uint8_t)(value >> (i * 8));
}

static void vr41xx_seed_latch32(uint8_t *regs, uint32_t off, uint32_t value)
{
	regs[off + 0] = (uint8_t)(value & 0xff);
	regs[off + 1] = (uint8_t)((value >> 8) & 0xff);
	regs[off + 2] = (uint8_t)((value >> 16) & 0xff);
	regs[off + 3] = (uint8_t)((value >> 24) & 0xff);
}

static void log_resume_probe_window(struct cpu *cpu, const char *label,
	const char *kind, uint32_t center, int start_rel, int stop_rel)
{
	int rel;

	for (rel = start_rel; rel <= stop_rel; rel += 4) {
		unsigned char buf[4];
		uint32_t va32 = center + (uint32_t) rel;
		uint64_t va = (uint64_t)(int64_t)(int32_t)va32;

		if (cpu->memory_rw(cpu, cpu->mem, va, buf, sizeof(buf), MEM_READ,
		    CACHE_DATA | NO_EXCEPTIONS)) {
			uint32_t w = (uint32_t)buf[0]
			    | ((uint32_t)buf[1] << 8)
			    | ((uint32_t)buf[2] << 16)
			    | ((uint32_t)buf[3] << 24);
			fprintf(stderr,
			    "[WINCE_MMIO_PC] %s %s_rel=%+03d va=0x%08x"
			    " word=%08x\n",
			    label,
			    kind,
			    rel,
			    va32,
			    w);
		} else {
			fprintf(stderr,
			    "[WINCE_MMIO_PC] %s %s_rel=%+03d va=0x%08x"
			    " word=????????\n",
			    label,
			    kind,
			    rel,
			    va32);
		}
	}
}

static void log_resume_dispatch_state(struct cpu *cpu, const char *label)
{
	uint32_t pc = (uint32_t)cpu->pc;
	uint32_t ra = (uint32_t)cpu->cd.mips.gpr[31];
	uint32_t sp = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
	uint32_t gp = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_GP];
	uint32_t a0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
	uint32_t a1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1];
	uint32_t a2 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2];
	uint32_t a3 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3];
	uint32_t v0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0];
	uint32_t v1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V1];
	uint32_t t0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T0];
	uint32_t t1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T1];
	uint32_t t2 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T2];
	uint32_t t3 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T3];
	uint32_t t4 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T4];
	uint32_t t5 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T5];
	uint32_t t6 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T6];
	uint32_t t7 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T7];
	uint32_t t8 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T8];
	uint32_t t9 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T9];

	fprintf(stderr,
	    "[WINCE_MMIO_DISPATCH] label=%s pc=0x%08x ra=0x%08x sp=0x%08x"
	    " gp=0x%08x a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x"
	    " v0=0x%08x v1=0x%08x t0=0x%08x t1=0x%08x"
	    " t2=0x%08x t3=0x%08x t4=0x%08x t5=0x%08x"
	    " t6=0x%08x t7=0x%08x t8=0x%08x t9=0x%08x\n",
	    label,
	    pc,
	    ra,
	    sp,
	    gp,
	    a0,
	    a1,
	    a2,
	    a3,
	    v0,
	    v1,
	    t0,
	    t1,
	    t2,
	    t3,
	    t4,
	    t5,
	    t6,
	    t7,
	    t8,
	    t9);

	log_resume_probe_window(cpu, label, "slot0_root", 0x800aad2c, -16, 96);
	log_resume_probe_window(cpu, label, "slot0_wrapper", 0x800aada0, -16, 48);
	log_resume_probe_window(cpu, label, "slot0_helper", 0x800aae20, -16, 48);
	log_resume_probe_window(cpu, label, "slot0_follow", 0x800aae48, -16, 64);
	log_resume_probe_window(cpu, label, "dispatch_ptrs", 0x80660170, 0, 44);
	log_resume_probe_window(cpu, label, "dispatch_slots", 0x80660310, 0, 124);
	log_resume_probe_window(cpu, label, "dispatch_slots_hi", 0x80660320, 0, 60);
	log_resume_probe_window(cpu, label, "callback_page", 0xa0051680, 0, 252);
	log_resume_probe_window(cpu, label, "callback_page_1740", 0xa0051740, 0, 60);
	log_resume_probe_window(cpu, label, "dispatch_page_1000", 0xa0051000, 0, 60);
	log_resume_probe_window(cpu, label, "bootctx_stub", 0xa00063d0, 0, 60);
	log_resume_probe_window(cpu, label, "obj_table_66bfc0", 0x8066bfc0, 0, 60);
	if (a0 != 0)
		log_resume_probe_window(cpu, label, "arg_a0",
		    a0 & ~0x1fu, 0, 60);
	if (a1 != 0)
		log_resume_probe_window(cpu, label, "arg_a1",
		    a1 & ~0x1fu, 0, 60);
	if (a2 != 0)
		log_resume_probe_window(cpu, label, "arg_a2",
		    a2 & ~0x1fu, 0, 60);
	if (a3 != 0)
		log_resume_probe_window(cpu, label, "arg_a3",
		    a3 & ~0x1fu, 0, 60);
}

static void maybe_log_resume_mmio_probe(struct cpu *cpu,
	uint64_t relative_addr, int writeflag, uint64_t value)
{
	static const struct {
		uint32_t pc;
		const char *label;
		int code_start_rel;
		int code_stop_rel;
	} probes[] = {
		{ 0x8007826c, "resume_mmio_7826c", -16, 32 },
		{ 0x8007a114, "resume_mmio_7a114", -16, 32 },
		{ 0x800a5e94, "resume_mmio_a5e94", -16, 32 },
		{ 0x80079724, "resume_mmio_79724", -16, 32 },
		{ 0x800aad2c, "dispatch_mmio_aad2c", -32, 64 },
		{ 0x800aae20, "dispatch_mmio_aae20", -32, 64 },
		{ 0x800aae6c, "dispatch_mmio_aae6c", -32, 64 },
		{ 0x800a7b3c, "resume_poll_7b3c", -32, 64 },
		{ 0x800a7b64, "resume_poll_7b64", -32, 64 },
		{ 0x800a7bc4, "resume_poll_7bc4", -32, 64 },
	};
	static uint32_t logged_mask = 0;
	uint32_t pc = (uint32_t)cpu->pc;
	size_t i;

	for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
		uint32_t bit = 1u << i;
		uint32_t ra = (uint32_t)cpu->cd.mips.gpr[31];
		uint32_t sp = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
		int rel;

		if (probes[i].pc != pc)
			continue;
		if (logged_mask & bit)
			return;
		logged_mask |= bit;

		fprintf(stderr,
		    "[WINCE_MMIO_PC] label=%s pc=0x%08x off=0x%03" PRIx64
		    " op=%c value=0x%04" PRIx64
		    " ra=0x%08x sp=0x%08x s0=0x%08x s1=0x%08x s2=0x%08x\n",
		    probes[i].label,
		    pc,
		    relative_addr,
		    writeflag == MEM_WRITE ? 'W' : 'R',
		    value,
		    ra,
		    sp,
		    (uint32_t)cpu->cd.mips.gpr[16],
		    (uint32_t)cpu->cd.mips.gpr[17],
		    (uint32_t)cpu->cd.mips.gpr[18]);

		log_resume_probe_window(cpu, probes[i].label, "code", pc,
		    probes[i].code_start_rel, probes[i].code_stop_rel);
		if (ra != 0) {
			uint32_t caller = ra - 8u;

			fprintf(stderr,
			    "[WINCE_MMIO_PC] %s caller=0x%08x ra=0x%08x\n",
			    probes[i].label,
			    caller,
			    ra);
			log_resume_probe_window(cpu, probes[i].label, "caller",
			    caller, -16, 24);
		}
		if (sp != 0) {
			fprintf(stderr,
			    "[WINCE_MMIO_PC] %s stack_center=0x%08x\n",
			    probes[i].label,
			    sp);
			log_resume_probe_window(cpu, probes[i].label, "stack",
			    sp, -16, 48);
		}
		if (pc == 0x800aad2c || pc == 0x800aae20 || pc == 0x800aae6c)
			log_resume_dispatch_state(cpu, probes[i].label);
		return;
	}
}

static void maybe_log_resume_setup_state(struct cpu *cpu,
	struct vr41xx_data *d, uint64_t relative_addr, int writeflag,
	uint64_t value)
{
	static const struct {
		uint32_t pc;
		const char *label;
	} probes[] = {
		{ 0xa007957c, "pre_replay_sdramu_957c" },
		{ 0x800a5e94, "resume_setup_5e94" },
		{ 0x800a5fd0, "resume_setup_5fd0" },
	};
	static uint32_t logged_mask = 0;
	uint32_t pc = (uint32_t)cpu->pc;
	size_t i;

	for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
		uint32_t bit = 1u << i;

		if (probes[i].pc != pc)
			continue;
		if (logged_mask & bit)
			return;
		logged_mask |= bit;

		fprintf(stderr,
		    "[WINCE_VR41XX_SETUP] label=%s pc=0x%08x off=0x%03" PRIx64
		    " op=%c value=0x%04" PRIx64
		    " dmaau20=0x%08" PRIx64 " dmaau30=0x%08" PRIx64
		    " dcu40=0x%08" PRIx64 " dcu44=0x%08" PRIx64
		    " cmu60=0x%04x pmu_c0=0x%04x pmu_c2=0x%04x"
		    " pmu_c8=0x%04x pmu_cc=0x%04x sdramu400=0x%08" PRIx64
		    " sdramu404=0x%08" PRIx64 " sdramu408=0x%08" PRIx64 "\n",
		    probes[i].label,
		    pc,
		    relative_addr,
		    writeflag == MEM_WRITE ? 'W' : 'R',
		    value,
		    vr41xx_latch_read(d->dmaau_regs, 0x00, 4),
		    vr41xx_latch_read(d->dmaau_regs, 0x10, 4),
		    vr41xx_latch_read(d->dcu_regs, 0x00, 4),
		    vr41xx_latch_read(d->dcu_regs, 0x04, 4),
		    d->cmu.clkmsk,
		    d->pmu.pmuintreg,
		    d->pmu.pmucntreg,
		    d->pmu.pmuwaitreg,
		    d->pmu.pmudivreg,
		    vr41xx_latch_read(d->sdramu_regs, 0x00, 4),
		    vr41xx_latch_read(d->sdramu_regs, 0x04, 4),
		    vr41xx_latch_read(d->sdramu_regs, 0x08, 4));
		return;
	}
}

static void maybe_consume_wince_replay_etimer(struct cpu *cpu,
	struct vr41xx_data *d, uint64_t relative_addr, int writeflag)
{
	if (cpu == NULL || d == NULL || writeflag != MEM_WRITE)
		return;
	if (!wince_boot_replay_full_active(cpu->machine))
		return;

	/*
	 * The replay-only ETIMER path now reaches the real helper at
	 * 0x800AAE1C..0x800AAE40, which clears GIUINTENL bit1 and
	 * PMUCNTREG bit13 before returning. On real hardware this helper
	 * appears to retire the forwarded timer source; in the emulator,
	 * the synthetic pending_timer_interrupts queue keeps growing and
	 * sysint1 bit 3 never drops. Consume the queued ETIMER here once
	 * to test whether the remaining loop is just our internal latch
	 * model.
	 */
	if ((uint32_t)cpu->pc != 0x800aae3c || relative_addr != 0x0c2)
		return;
	if ((d->sysint1 & (1 << VRIP_INTR_ETIMER)) == 0
	    && d->pending_timer_interrupts == 0)
		return;

	fprintf(stderr,
	    "[WINCE_ETIMER] replay-consume pc=0x%08" PRIx64
	    " pending_before=%d sysint1_before=0x%04x rtcint_before=0x%04x\n",
	    (uint64_t)cpu->pc,
	    d->pending_timer_interrupts,
	    d->sysint1,
	    d->rtc.rtcint);

	d->pending_timer_interrupts = 0;
	d->sysint1 &= ~(1 << VRIP_INTR_ETIMER);
	d->rtc.rtcint &= (uint16_t)~RTCINT_ELAPSEDTIME_INT;
	d->replay_etimer_suppressed = 1;
	INTERRUPT_DEASSERT(d->timer_irq);
	wince_boot_note_replay_etimer_consumed(cpu->machine, cpu);
}

static void maybe_rearm_wince_replay_etimer(struct cpu *cpu,
	struct vr41xx_data *d, uint64_t relative_addr, int writeflag)
{
	if (cpu == NULL || d == NULL || writeflag != MEM_WRITE)
		return;
	if (!d->replay_etimer_suppressed)
		return;
	if (!wince_boot_replay_full_active(cpu->machine))
		return;
	if (!((relative_addr >= 0x108 && relative_addr <= 0x126)
	    || relative_addr == 0x0d0
	    || relative_addr == 0x0d2
	    || relative_addr == 0x13e)) {
		return;
	}

	d->replay_etimer_suppressed = 0;
	fprintf(stderr,
	    "[WINCE_ETIMER] replay-rearm pc=0x%08" PRIx64
	    " off=0x%03" PRIx64 " pending=0x%04x rtcint=0x%04x\n",
	    (uint64_t)cpu->pc,
	    relative_addr,
	    d->pending_timer_interrupts,
	    d->rtc.rtcint);
}


/*
 *  vr41xx_vrip_interrupt_assert():
 *  vr41xx_vrip_interrupt_deassert():
 */ 
void vr41xx_vrip_interrupt_assert(struct interrupt *interrupt)
{
	struct vr41xx_data *d = (struct vr41xx_data *) interrupt->extra;
	int line = interrupt->line;
	if (line < 16)
		d->sysint1 |= (1 << line);
	else
		d->sysint2 |= (1 << (line-16));
	if ((d->sysint1 & d->msysint1) | (d->sysint2 & d->msysint2))
                INTERRUPT_ASSERT(d->cpu_irq);
}
void vr41xx_vrip_interrupt_deassert(struct interrupt *interrupt)
{
	struct vr41xx_data *d = (struct vr41xx_data *) interrupt->extra;
	int line = interrupt->line;
	if (line < 16)
		d->sysint1 &= ~(1 << line);
	else
		d->sysint2 &= ~(1 << (line-16));
	if (!(d->sysint1 & d->msysint1) && !(d->sysint2 & d->msysint2))
                INTERRUPT_DEASSERT(d->cpu_irq);
}


/*
 *  vr41xx_giu_interrupt_assert():
 *  vr41xx_giu_interrupt_deassert():
 */ 
void vr41xx_giu_interrupt_assert(struct interrupt *interrupt)
{
	struct vr41xx_data *d = (struct vr41xx_data *) interrupt->extra;
	int line = interrupt->line;
	d->giuint |= (1 << line);
	if (d->giuint & d->giumask)
                INTERRUPT_ASSERT(d->giu_irq);
}
void vr41xx_giu_interrupt_deassert(struct interrupt *interrupt)
{
	struct vr41xx_data *d = (struct vr41xx_data *) interrupt->extra;
	int line = interrupt->line;
	d->giuint &= ~(1 << line);
	if (!(d->giuint & d->giumask))
                INTERRUPT_DEASSERT(d->giu_irq);
}


static void recalc_kiu_int_assert(struct cpu *cpu, struct vr41xx_data *d)
{
	if (d->kiu_int_assert != d->old_kiu_int_assert) {
		d->old_kiu_int_assert = d->kiu_int_assert;
		if (d->kiu_int_assert != 0)
			INTERRUPT_ASSERT(d->kiu_irq);
		else
			INTERRUPT_DEASSERT(d->kiu_irq);
	}
}


/*
 *  vr41xx_keytick():
 */
static void vr41xx_keytick(struct cpu *cpu, struct vr41xx_data *d)
{
	int keychange = 0;

	/*
	 *  Keyboard input:
	 *
	 *  Hardcoded for MobilePro. (See NetBSD's hpckbdkeymap.h for
	 *  info on other keyboard layouts. mobilepro780_keytrans is the
	 *  one used here.)
	 *
	 *  TODO: Make this work with "any" keyboard layout.
	 *
	 *  ofs 0:
	 *	8000='o' 4000='.' 2000=DOWN  1000=UP
	 *	 800=';'  400='''  200='['    100=?
	 *	  80='l'   40=CR    20=RIGHT   10=LEFT
	 *	   8='/'    4='\'    2=']'      1=SPACE
	 *  ofs 2:
	 *	8000='a' 4000='s' 2000='d' 1000='f'
	 *	 800='`'  400='-'  200='='  100=?
	 *	  80='z'   40='x'   20='c'   10='v'
	 *	   8=?      4=?      2=?
	 *  ofs 4:
	 *	8000='9' 4000='0' 2000=?   1000=?
	 *	 800='b'  400='n'  200='m'  100=','
	 *	  80='q'   40='w'   20='e'   10='r'
	 *	   8='5'    4='6'    2='7'    1='8'
	 *  ofs 6:
	 *	8000=ESC 4000=DEL 2000=CAPS 1000=?
	 *	 800='t'  400='y'  200='u'   100='i'
	 *	  80='1'   40='2'   20='3'    10='4'
	 *	   8='g'    4='h'    2='j'     1='k'
	 *  ofs 8:
	 *                      200=ALT_L
	 *	  80=    40=TAB  20='p' 10=BS
	 *	   8=     4=      2=     1=ALT_R
	 *  ofs a:
	 *	800=SHIFT 4=CTRL
	 *
	 *
	 *  The following are for the IBM WorkPad Z50:
	 *  (Not yet implemented, TODO)
	 *
	 *  00    f1      f3      f5      f7      f9      -       -       f11
	 *  08    f2      f4      f6      f8      f10     -       -       f12
	 *  10    '       [       -       0       p       ;       up      /
	 *  18    -       -       -       9       o       l       .       -
	 *  20    left    ]       =       8       i       k       ,       -
	 *  28    h       y       6       7       u       j       m       n
	 *  30    -       bs      num     del     -       \       ent     sp
	 *  38    g       t       5       4       r       f       v       b
	 *  40    -       -       -       3       e       d       c       right
	 *  48    -       -       -       2       w       s       x       down
	 *  50    esc     tab     ~       1       q       a       z       -
	 *  58    menu    Ls      Lc      Rc      La      Ra      Rs      -
	 */

	if (d->d0 != 0 || d->d1 != 0 || d->d2 != 0 ||
	    d->d3 != 0 || d->d4 != 0 || d->d5 != 0)
		keychange = 1;

	/*  Release all keys:  */
	if (!d->dont_clear_next) {
		d->d0 = d->d1 = d->d2 = d->d3 = d->d4 = d->d5 = 0;
	} else
		d->dont_clear_next = 0;

	if (console_charavail(d->kiu_console_handle)) {
		char ch = console_readchar(d->kiu_console_handle);

		if (d->escape_state > 0) {
			switch (d->escape_state) {
			case 1:	/*  expecting a [  */
				d->escape_state = 0;
				if (ch == '[')
					d->escape_state = 2;
				break;
			case 2:	/*  cursor keys etc:  */
				/*  Ugly hack for Mobilepro770:  */
				if (cpu->machine->machine_subtype ==
				    MACHINE_HPCMIPS_NEC_MOBILEPRO_770) {
					switch (ch) {
					case 'A': d->d0 = 0x2000; break;
					case 'B': d->d0 = 0x20; break;
					case 'C': d->d0 = 0x1000; break;
					case 'D': d->d0 = 0x10; break;
					default:  fatal("[ vr41xx kiu: unimpl"
					    "emented escape 0x%02 ]\n", ch);
					}
				} else {
					switch (ch) {
					case 'A': d->d0 = 0x1000; break;
					case 'B': d->d0 = 0x2000; break;
					case 'C': d->d0 = 0x20; break;
					case 'D': d->d0 = 0x10; break;
					default:  fatal("[ vr41xx kiu: unimpl"
					    "emented escape 0x%02 ]\n", ch);
					}
				}
				d->escape_state = 0;
			}
		} else switch (ch) {
		case '+':	console_makeavail(d->kiu_console_handle, '=');
				d->d5 = 0x800; break;
		case '_':	console_makeavail(d->kiu_console_handle, '-');
				d->d5 = 0x800; break;
		case '<':	console_makeavail(d->kiu_console_handle, ',');
				d->d5 = 0x800; break;
		case '>':	console_makeavail(d->kiu_console_handle, '.');
				d->d5 = 0x800; break;
		case '{':	console_makeavail(d->kiu_console_handle, '[');
				d->d5 = 0x800; break;
		case '}':	console_makeavail(d->kiu_console_handle, ']');
				d->d5 = 0x800; break;
		case ':':	console_makeavail(d->kiu_console_handle, ';');
				d->d5 = 0x800; break;
		case '"':	console_makeavail(d->kiu_console_handle, '\'');
				d->d5 = 0x800; break;
		case '|':	console_makeavail(d->kiu_console_handle, '\\');
				d->d5 = 0x800; break;
		case '?':	console_makeavail(d->kiu_console_handle, '/');
				d->d5 = 0x800; break;

		case '!':	console_makeavail(d->kiu_console_handle, '1');
				d->d5 = 0x800; break;
		case '@':	console_makeavail(d->kiu_console_handle, '2');
				d->d5 = 0x800; break;
		case '#':	console_makeavail(d->kiu_console_handle, '3');
				d->d5 = 0x800; break;
		case '$':	console_makeavail(d->kiu_console_handle, '4');
				d->d5 = 0x800; break;
		case '%':	console_makeavail(d->kiu_console_handle, '5');
				d->d5 = 0x800; break;
		case '^':	console_makeavail(d->kiu_console_handle, '6');
				d->d5 = 0x800; break;
		case '&':	console_makeavail(d->kiu_console_handle, '7');
				d->d5 = 0x800; break;
		case '*':	console_makeavail(d->kiu_console_handle, '8');
				d->d5 = 0x800; break;
		case '(':	console_makeavail(d->kiu_console_handle, '9');
				d->d5 = 0x800; break;
		case ')':	console_makeavail(d->kiu_console_handle, '0');
				d->d5 = 0x800; break;

		case '1':	d->d3 = 0x80; break;
		case '2':	d->d3 = 0x40; break;
		case '3':	d->d3 = 0x20; break;
		case '4':	d->d3 = 0x10; break;
		case '5':	d->d2 = 0x08; break;
		case '6':	d->d2 = 0x04; break;
		case '7':	d->d2 = 0x02; break;
		case '8':	d->d2 = 0x01; break;
		case '9':	d->d2 = 0x8000; break;
		case '0':	d->d2 = 0x4000; break;

		case ';':	d->d0 = 0x800; break;
		case '\'':	d->d0 = 0x400; break;
		case '[':	d->d0 = 0x200; break;
		case '/':	d->d0 = 0x8; break;
		case '\\':	d->d0 = 0x4; break;
		case ']':	d->d0 = 0x2; break;

		case 'a':	d->d1 = 0x8000; break;
		case 'b':	d->d2 = 0x800; break;
		case 'c':	d->d1 = 0x20; break;
		case 'd':	d->d1 = 0x2000; break;
		case 'e':	d->d2 = 0x20; break;
		case 'f':	d->d1 = 0x1000; break;
		case 'g':	d->d3 = 0x8; break;
		case 'h':	d->d3 = 0x4; break;
		case 'i':	d->d3 = 0x100; break;
		case 'j':	d->d3 = 0x2; break;
		case 'k':	d->d3 = 0x1; break;
		case 'l':	d->d0 = 0x80; break;
		case 'm':	d->d2 = 0x200; break;
		case 'n':	d->d2 = 0x400; break;
		case 'o':	d->d0 = 0x8000; break;
		case 'p':	d->d4 = 0x20; break;
		case 'q':	d->d2 = 0x80; break;
		case 'r':	d->d2 = 0x10; break;
		case 's':	d->d1 = 0x4000; break;
		case 't':	d->d3 = 0x800; break;
		case 'u':	d->d3 = 0x200; break;
		case 'v':	d->d1 = 0x10; break;
		case 'w':	d->d2 = 0x40; break;
		case 'x':	d->d1 = 0x40; break;
		case 'y':	d->d3 = 0x400; break;
		case 'z':	d->d1 = 0x80; break;

		case ',':	d->d2 = 0x100; break;
		case '.':	d->d0 = 0x4000; break;
		case '-':	d->d1 = 0x400; break;
		case '=':	d->d1 = 0x200; break;

		case '\r':
		case '\n':	d->d0 = 0x40; break;
		case ' ':	d->d0 = 0x01; break;
		case '\b':	d->d4 = 0x10; break;

		case 27:	d->escape_state = 1; break;

		default:
			/*  Shifted:  */
			if (ch >= 'A' && ch <= 'Z') {
				console_makeavail(d->kiu_console_handle,
				    ch + 32);
				d->d5 = 0x800;
				d->dont_clear_next = 1;
				break;
			}

			/*  CTRLed:  */
			if (ch >= 1 && ch <= 26) {
				console_makeavail(d->kiu_console_handle,
				    ch + 96);
				d->d5 = 0x4;
				d->dont_clear_next = 1;
				break;
			}
		}

		if (d->escape_state == 0)
			keychange = 1;
	}

	if (keychange) {
		/*  4=lost data, 2=data complete, 1=key input detected  */
		d->kiu_int_assert |= 3;
		recalc_kiu_int_assert(cpu, d);
	}
}


/*
 *  timer_tick():
 */
static void timer_tick(struct timer *timer, void *extra)
{
	struct vr41xx_data *d = (struct vr41xx_data *) extra;

	if (d->replay_etimer_suppressed)
		return;
	d->pending_timer_interrupts ++;
	d->rtc.rtcint |= RTCINT_RTCLONG1_INT;
}


DEVICE_TICK(vr41xx)
{
	struct vr41xx_data *d = (struct vr41xx_data *) extra;
	int timer_allowed;

	wince_boot_on_vr41xx_tick(cpu->machine, cpu);
	timer_allowed = wince_boot_timer_irq_allowed(cpu->machine, cpu) ? 1 : 0;

	/*
	 * Model RTCL1 as a direct CPU interrupt pulse. WinCE programs the
	 * periodic source once and then services recurring timer callbacks
	 * without writing RTCINTREG on every tick, so holding the line level-
	 * asserted until a register ack leaves the kernel spinning forever.
	 * Keep a small backlog when interrupts are masked, but retire one
	 * queued event as soon as we emit the pulse.
	 */
	if (d->rtcl1_irq_asserted) {
		INTERRUPT_DEASSERT(d->rtcl1_irq);
		d->rtcl1_irq_asserted = 0;
	}
	if (!timer_allowed && d->pending_timer_interrupts > 1)
		d->pending_timer_interrupts = 1;
	if (d->pending_timer_interrupts > 0 && timer_allowed) {
		INTERRUPT_ASSERT(d->rtcl1_irq);
		d->rtcl1_irq_asserted = 1;
		d->pending_timer_interrupts --;
		{
			static int timer_fire_diag = 0;
			if (timer_fire_diag < 3) {
				uint32_t st = cpu->cd.mips.coproc[0]->
				    reg[COP0_STATUS];
				uint32_t ca = cpu->cd.mips.coproc[0]->
				    reg[COP0_CAUSE];
				uint32_t bva = cpu->cd.mips.coproc[0]->
				    reg[COP0_BADVADDR];
				fprintf(stderr,
				    "[VR41XX_TICK] timer fire! pending=%d"
				    " sysint1=0x%04x msysint1=0x%04x"
				    " Status=0x%08x Cause=0x%08x"
				    " BadVA=0x%08x"
				    " PC=0x%08" PRIx64 "\n",
				    d->pending_timer_interrupts,
				    d->sysint1, d->msysint1,
				    st, ca, bva, (uint64_t)cpu->pc);
				timer_fire_diag++;
			}
		}
	}

	/*  Advance VR4131 RTC elapsed-time counter each tick:  */
	rtc_tick(&d->rtc, 1);

	/*
	 *  ETIME/ECMP compare interrupt — used by Linux 2.6 VR41xx timer
	 *  and WinCE elapsed time timer.
	 *  The 2.6 kernel programs ECMP and expects an interrupt when
	 *  ETIME reaches ECMP.  Only use this path when the RTCL1 timer
	 *  is not active (d->timer == NULL); the 2.4 kernel creates the
	 *  RTCL1 timer via offset 0xd0 and the initial etime >> ecmp
	 *  would cause a spurious interrupt storm.
	 */
	if (d->timer == NULL &&
	    (d->rtc.rtcint & RTCINT_ELAPSEDTIME_INT) &&
	    timer_allowed)
		INTERRUPT_ASSERT(d->timer_irq);

	/* No wake signal for hibernate — see idle handler */

	/*  KIU keyboard tick — disabled (no X11 keyboard input)  */
}


/*
 *  vr41xx_kiu():
 *
 *  Keyboard Interface Unit. Return value is "odata".
 *  (See NetBSD's vrkiu.c for more info.)
 */
static uint64_t vr41xx_kiu(struct cpu *cpu, int ofs, uint64_t idata,
	int writeflag, struct vr41xx_data *d)
{
	uint64_t odata = 0;

	switch (ofs) {
	case KIUDAT0:
		odata = d->d0; break;
	case KIUDAT1:
		odata = d->d1; break;
	case KIUDAT2:
		odata = d->d2; break;
	case KIUDAT3:
		odata = d->d3; break;
	case KIUDAT4:
		odata = d->d4; break;
	case KIUDAT5:
		odata = d->d5; break;
	case KIUSCANREP:
		if (writeflag == MEM_WRITE) {
			debug("[ vr41xx KIU: setting KIUSCANREP to 0x%04x ]\n",
			    (int)idata);
			/*  TODO  */
		} else
			fatal("[ vr41xx KIU: unimplemented read from "
			    "KIUSCANREP ]\n");
		break;
	case KIUSCANS:
		if (writeflag == MEM_WRITE) {
			debug("[ vr41xx KIU: write to KIUSCANS: 0x%04x: TODO"
			    " ]\n", (int)idata);
			/*  TODO  */
		} else
			debug("[ vr41xx KIU: unimplemented read from "
			    "KIUSCANS ]\n");
		break;
	case KIUINT:
		/*  Interrupt. A wild guess: zero-on-write  */
		if (writeflag == MEM_WRITE) {
			d->kiu_int_assert &= ~idata;
		} else {
			odata = d->kiu_int_assert;
		}
		recalc_kiu_int_assert(cpu, d);
		break;
	case KIURST:
		/*  Reset.  */
		break;
	default:
		if (writeflag == MEM_WRITE)
			debug("[ vr41xx KIU: unimplemented write to offset "
			    "0x%x, data=0x%016" PRIx64" ]\n", ofs,
			    (uint64_t) idata);
		else
			debug("[ vr41xx KIU: unimplemented read from offset "
			    "0x%x ]\n", ofs);
	}

	return odata;
}


DEVICE_ACCESS(vr41xx)
{
	struct vr41xx_data *d = (struct vr41xx_data *) extra;
	uint64_t idata = 0, odata = 0;
	int revision = 0;

	if (writeflag == MEM_WRITE)
		idata = memory_readmax64(cpu, data, len);

	maybe_log_resume_mmio_probe(cpu, relative_addr, writeflag,
	    writeflag == MEM_WRITE ? idata : 0);
	maybe_rearm_wince_replay_etimer(cpu, d, relative_addr, writeflag);

	// int regnr = relative_addr / sizeof(uint64_t);

	/*  KIU ("Keyboard Interface Unit") is handled separately.  */
	if (relative_addr >= d->kiu_offset &&
	    relative_addr < d->kiu_offset + 0x20) {
		odata = vr41xx_kiu(cpu, relative_addr - d->kiu_offset,
		    idata, writeflag, d);
		goto ret;
	}

	/*  TODO: Maybe these should be handled separately as well?  */
	if (d->cpumodel == 4131) {
		if (relative_addr >= 0x20 && relative_addr < 0x40) {
			uint32_t off = (uint32_t)(relative_addr - 0x20);
			if (writeflag == MEM_READ)
				odata = vr41xx_latch_read(d->dmaau_regs, off,
				    (unsigned)len);
			else
				vr41xx_latch_write(d->dmaau_regs, off,
				    (unsigned)len, idata);
			goto log_access;
		}
		if (relative_addr >= 0x40 && relative_addr < 0x60) {
			uint32_t off = (uint32_t)(relative_addr - 0x40);
			if (writeflag == MEM_READ)
				odata = vr41xx_latch_read(d->dcu_regs, off,
				    (unsigned)len);
			else
				vr41xx_latch_write(d->dcu_regs, off,
				    (unsigned)len, idata);
			goto log_access;
		}
		if (relative_addr >= 0x400 && relative_addr < 0x410) {
			uint32_t off = (uint32_t)(relative_addr - 0x400);
			if (writeflag == MEM_READ)
				odata = vr41xx_latch_read(d->sdramu_regs, off,
				    (unsigned)len);
			else
				vr41xx_latch_write(d->sdramu_regs, off,
				    (unsigned)len, idata);
			goto log_access;
		}
	}

	switch (relative_addr) {

	/*  BCU:  0x00 .. 0x1c  */
	case BCUREVID_REG_W:	/*  0x010  */
	case BCU81REVID_REG_W:	/*  0x014  */
		/*
		 *  TODO?  Linux seems to read 0x14. The lowest bits are
		 *  a divisor for PClock, bits 8 and up seem to be a
		 *  divisor for VTClock (relative to PClock?)...
		 */
		switch (d->cpumodel) {
		case 4131:	revision = BCUREVID_RID_4131; break;
		case 4122:	revision = BCUREVID_RID_4122; break;
		case 4121:	revision = BCUREVID_RID_4121; break;
		case 4111:	revision = BCUREVID_RID_4111; break;
		case 4102:	revision = BCUREVID_RID_4102; break;
		case 4101:	revision = BCUREVID_RID_4101; break;
		case 4181:	revision = BCUREVID_RID_4181; break;
		}
		odata = (revision << BCUREVID_RIDSHFT) | 0x020c;
		break;
	case BCU81CLKSPEED_REG_W:	/*  0x018  */
		/*
		 *  TODO: Implement this for ALL cpu types:
		 */
		odata = BCUCLKSPEED_DIVT4 << BCUCLKSPEED_DIVTSHFT;
		break;

	/*  DMAAU:  0x20 .. 0x3c  */

	/*  DCU:  0x40 .. 0x5c  */

	/*  CMU:  0x60 .. 0x7c  */
	case 0x60:
	case 0x62:
		if (d->cpumodel == 4131) {
			uint32_t cmu_off = (uint32_t)(relative_addr - 0x60);
			if (writeflag == MEM_READ)
				odata = cmu_read(&d->cmu, cmu_off, (unsigned)len);
			else
				cmu_write(&d->cmu, cmu_off, (unsigned)len,
				    (uint32_t)idata);
			break;
		}
		goto unhandled;

	/*  ICU:  0x80 .. 0xbc  */
	case 0x80:	/*  Level 1 system interrupt reg 1...  */
		if (writeflag == MEM_READ)
			odata = d->sysint1;
		else {
			/*  TODO: clear-on-write-one?  */
			d->sysint1 &= ~idata;
			d->sysint1 &= 0xffff;
		}
		break;
	case 0x88:
		if (writeflag == MEM_READ)
			odata = d->giuint;
		else
			d->giuint &= ~idata;
		break;
	case 0x8c:
		if (writeflag == MEM_READ)
			odata = d->msysint1;
		else {
			/*
			 *  Let the kernel control MSYSINT1 fully.
			 *  Previously ETIMER (bit 3) was force-enabled
			 *  for Linux 2.6, but this prevents WinCE from
			 *  managing its own timer mask — the WinCE
			 *  interrupt dispatch loop checks SYSINT1 &
			 *  MSYSINT1 and loops forever if ETIMER is
			 *  stuck enabled while the timer fires.
			 */
			d->msysint1 = idata;
		}
		break;
	case 0x94:
		if (writeflag == MEM_READ)
			odata = d->giumask;
		else
			d->giumask = idata;
		break;
	case 0xa0:	/*  Level 1 system interrupt reg 2...  */
		if (writeflag == MEM_READ)
			odata = d->sysint2;
		else {
			/*  TODO: clear-on-write-one?  */
			d->sysint2 &= ~idata;
			d->sysint2 &= 0xffff;
		}
		break;
	case 0xa6:
		if (writeflag == MEM_READ)
			odata = d->msysint2;
		else
			d->msysint2 = idata;
		break;

	/*  VR4131 PMU lives at 0xc0; older VR41xx chips use this as RTC.  */
	case 0xc0:
	case 0xc2:
	case 0xc4:
	case 0xc6:
	case 0xc8:
	case 0xcc:
		if (d->cpumodel == 4131) {
			uint32_t pmu_off = (uint32_t)(relative_addr - 0xc0);
			if (writeflag == MEM_READ)
				odata = pmu_read(&d->pmu, pmu_off, (unsigned)len);
			else
				pmu_write(&d->pmu, pmu_off, (unsigned)len,
				    (uint32_t)idata);
			maybe_consume_wince_replay_etimer(cpu, d, relative_addr,
			    writeflag);
			break;
		}
		if (relative_addr > 0xc4)
			goto unhandled;
		{
			struct timeval tv;
			gettimeofday(&tv, NULL);
			/*  Adjust time by 120 years and 29 days.  */
			tv.tv_sec += (int64_t) (120*365 + 29) * 24*60*60;

			switch (relative_addr) {
			case 0xc0:
				odata = (tv.tv_sec & 1) << 15;
				odata += (uint64_t)tv.tv_usec * 32768 / 1000000;
				break;
			case 0xc2:
				odata = (tv.tv_sec >> 1) & 0xffff;
				break;
			case 0xc4:
				odata = (tv.tv_sec >> 17) & 0xffff;
				break;
			}
		}
		break;

	case 0xd0:	/*  RTCL1_L_REG_W (older VR41xx)  */
	case 0x110:	/*  RTCL1_L_REG_W (VR4131 relocated)  */
		if (writeflag == MEM_WRITE && idata != 0) {
			int hz = RTCL1_L_HZ / idata;
			fprintf(stderr, "[VR41XX_TIMER] RTCL1 off=0x%03x"
			    " val=0x%04x → %d Hz timer=%p\n",
			    (int)relative_addr, (int)idata,
			    hz, (void*)d->timer);
			if (d->timer == NULL)
				d->timer = timer_add(hz, timer_tick, d);
			else
				timer_update_frequency(d->timer, hz);
			wince_boot_note_timer_config(cpu->machine, cpu,
			    relative_addr, idata);
		}
		if (d->cpumodel == 4131 && relative_addr == 0x110) {
			uint32_t rtc_off = (uint32_t)(relative_addr - 0x100);

			if (writeflag == MEM_READ)
				odata = rtc_read(&d->rtc, rtc_off,
				    (unsigned)len);
			else
				rtc_write(&d->rtc, rtc_off, (unsigned)len,
				    (uint32_t)idata);
		}
		break;
	case 0xd2:	/*  RTCL1_H_REG_W (older VR41xx)  */
	case 0x112:	/*  RTCL1_H_REG_W (VR4131 relocated)  */
		if (d->cpumodel == 4131 && relative_addr == 0x112) {
			uint32_t rtc_off = (uint32_t)(relative_addr - 0x100);

			if (writeflag == MEM_READ)
				odata = rtc_read(&d->rtc, rtc_off,
				    (unsigned)len);
			else
				rtc_write(&d->rtc, rtc_off, (unsigned)len,
				    (uint32_t)idata);
		}
		break;

	/*
	 *  VR4131 RTC block (offset 0x100-0x13e). On older VR41xx chips,
	 *  only the ETIME/ECMP subset lives elsewhere; on VR4131 the full
	 *  RTC/RTC2 window is relocated here.
	 */
	case 0x100:	/*  VR4131 ETIMELREG  */
	case 0x102:	/*  VR4131 ETIMEMREG  */
	case 0x104:	/*  VR4131 ETIMEHREG  */
	case 0x108:	/*  VR4131 ECMPLREG / older VR41xx GIUINTREG  */
	case 0x10a:	/*  VR4131 ECMPMREG  */
	case 0x10c:	/*  VR4131 ECMPHREG  */
	case 0x114:	/*  VR4131 RTCL1CNTLREG  */
	case 0x116:	/*  VR4131 RTCL1CNTHREG  */
	case 0x118:	/*  VR4131 RTCL2LREG  */
	case 0x11a:	/*  VR4131 RTCL2HREG  */
	case 0x11c:	/*  VR4131 RTCL2CNTLREG  */
	case 0x11e:	/*  VR4131 RTCL2CNTHREG  */
	case 0x120:	/*  VR4131 TCLKLREG  */
	case 0x122:	/*  VR4131 TCLKHREG  */
	case 0x124:	/*  VR4131 TCLKCNTLREG  */
	case 0x126:	/*  VR4131 TCLKCNTHREG  */
		if (d->cpumodel == 4131) {
			uint32_t rtc_off = (uint32_t)(relative_addr - 0x100);
			if (writeflag == MEM_READ)
				odata = rtc_read(&d->rtc, rtc_off, (unsigned)len);
			else
				rtc_write(&d->rtc, rtc_off, (unsigned)len,
				    (uint32_t)idata);
			break;
		}
		if (relative_addr == 0x108) {
			/*  Older VR41xx: GIU interrupt register  */
			if (writeflag == MEM_READ)
				odata = d->giuint;
			else
				d->giuint &= ~idata;
			break;
		}
		goto unhandled;

	case 0x13e:	/*  on 4181? / VR4131 RTCINTREG  */
	case 0x1de:	/*  on 4121?  */
		/*  RTC interrupt register...  */
		/*  Ack. only the timer sources whose RTCINT bits are written. */
		if (idata & RTCINT_RTCLONG1_INT) {
			INTERRUPT_DEASSERT(d->rtcl1_irq);
			d->rtcl1_irq_asserted = 0;
		}
		if (idata & RTCINT_ELAPSEDTIME_INT)
			INTERRUPT_DEASSERT(d->timer_irq);
		if (d->cpumodel == 4131 && relative_addr == 0x13e)
			rtc_write(&d->rtc, RTC_RTCINTREG, (unsigned)len,
			    (uint32_t)idata);
		break;

	/*
	 *  VR4131 GIU (GPIO) registers at 0x140-0x15C.
	 *  GIUPIODL (0x144) returns pin input state — WinCE checks this
	 *  to determine hardware/power status during OAL init.
	 */
	case 0x140:	/*  GIUIOSELL - I/O direction select low  */
	case 0x142:	/*  GIUIOSELH - I/O direction select high  */
	case 0x146:	/*  GIUPIODH - Pin I/O data high  */
	case 0x148:	/*  GIUINTSTATL  */
	case 0x14a:	/*  GIUINTSTATH  */
	case 0x14c:	/*  GIUINTENL  */
	case 0x14e:	/*  GIUINTENH  */
	case 0x150:	/*  GIUINTTYPL  */
	case 0x152:	/*  GIUINTTYPH  */
	case 0x154:	/*  GIUINTALSELL  */
	case 0x156:	/*  GIUINTALSELH  */
	case 0x158:	/*  GIUINTHTSELL  */
	case 0x15a:	/*  GIUINTHTSELH  */
		/*  Simple latch for most GIU registers  */
		{
			int idx = ((int)relative_addr - 0x140) / 2;
			if (idx >= 0 && idx < 16) {
				if (writeflag == MEM_WRITE)
					d->giu_regs[idx] = (uint16_t)idata;
				else
					odata = d->giu_regs[idx];
			}
		}
		break;
	case 0x144:	/*  GIUPIODL - Pin I/O data low  */
		if (writeflag == MEM_WRITE) {
			/*  Just latch output bits  */
		} else {
			/*
			 *  Return GPIO pin state for BE-300.
			 *  Bit 1 (GIU1): high = power button not pressed
			 *  Other bits: reflect warm-state hardware survey
			 */
			odata = 0x0490;
		}
		break;

	default:
	unhandled:
		if (writeflag == MEM_WRITE)
			debug("[ vr41xx: unimplemented write to address "
			    "0x%" PRIx64", data=0x%016" PRIx64" ]\n",
			    (uint64_t) relative_addr, (uint64_t) idata);
		else
			debug("[ vr41xx: unimplemented read from address "
			    "0x%" PRIx64" ]\n", (uint64_t) relative_addr);
	}

log_access:
	/*
	 *  Log all VR4131 register accesses during emulation so we can
	 *  trace WinCE's timer/interrupt setup. The latch-backed warm-state
	 *  windows above must flow through here too; otherwise the one-shot
	 *  resume setup probes never see them.
	 */
	maybe_log_resume_setup_state(cpu, d, relative_addr, writeflag,
	    writeflag == MEM_WRITE ? idata : odata);

	{
		static int vr41xx_log_count = 0;
		/* Only log NK.exe accesses (skip SPL at 0x80F0xxxx) */
		uint32_t norm_pc = (uint32_t)cpu->pc & 0x1FFFFFFFu;
		if (vr41xx_log_count < 10000 &&
		    norm_pc < 0x00F00000u) {
			fprintf(stderr, "[VR41XX] %s off=0x%03x val=0x%04x"
			    " PC=0x%08" PRIx64 "\n",
			    writeflag == MEM_WRITE ? "W" : "R",
			    (unsigned)relative_addr,
			    (unsigned)(writeflag == MEM_WRITE ? idata : odata),
			    (uint64_t)cpu->pc);
			vr41xx_log_count++;
		}
	}

ret:
	/*
	 *  Recalculate interrupt assertions:
	 */
	if (d->giuint & d->giumask)
                INTERRUPT_ASSERT(d->giu_irq);
	else
                INTERRUPT_DEASSERT(d->giu_irq);
	if ((d->sysint1 & d->msysint1) | (d->sysint2 & d->msysint2))
                INTERRUPT_ASSERT(d->cpu_irq);
	else
                INTERRUPT_DEASSERT(d->cpu_irq);

	if (writeflag == MEM_READ)
		memory_writemax64(cpu, data, len, odata);

	return 1;
}


/*
 *  dev_vr41xx_init():
 *
 *  machine->path is something like "machine[0]".
 */
struct vr41xx_data *dev_vr41xx_init(struct machine *machine,
	struct memory *mem, int cpumodel)
{
	struct vr41xx_data *d;
	uint64_t baseaddr = 0;
	char tmps[300];
	int i;

	CHECK_ALLOCATION(d = (struct vr41xx_data *) malloc(sizeof(struct vr41xx_data)));
	memset(d, 0, sizeof(struct vr41xx_data));

	/*  Connect to MIPS irq 2:  */
	snprintf(tmps, sizeof(tmps), "%s.cpu[%i].2",
	    machine->path, machine->bootstrap_cpu);
	INTERRUPT_CONNECT(tmps, d->cpu_irq);

	/*
	 *  Register VRIP interrupt lines 0..25:
	 */
	for (i=0; i<=25; i++) {
		struct interrupt templ;
		snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i",
		    machine->path, machine->bootstrap_cpu, i);
		memset(&templ, 0, sizeof(templ));
		templ.line = i;
		templ.name = tmps;
		templ.extra = d;
		templ.interrupt_assert = vr41xx_vrip_interrupt_assert;
		templ.interrupt_deassert = vr41xx_vrip_interrupt_deassert;
		interrupt_handler_register(&templ);
	}

	/*
	 *  Register GIU interrupt lines 0..31:
	 */
	for (i=0; i<32; i++) {
		struct interrupt templ;
		snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i.giu.%i",
		    machine->path, machine->bootstrap_cpu, VRIP_INTR_GIU, i);
		memset(&templ, 0, sizeof(templ));
		templ.line = i;
		templ.name = tmps;
		templ.extra = d;
		templ.interrupt_assert = vr41xx_giu_interrupt_assert;
		templ.interrupt_deassert = vr41xx_giu_interrupt_deassert;
		interrupt_handler_register(&templ);
	}

	d->cpumodel = cpumodel;

	/*  VR4131 RTC elapsed-time counter (read-assist for SPL polling):  */
	rtc_init(&d->rtc);
	cmu_init(&d->cmu);
	pmu_init(&d->pmu);
	/*
	 * Stable warm-state survey seeds for the otherwise-unimplemented
	 * DMAAU/DCU windows that WinCE touches during resume setup:
	 *   0x0f000020: 01fff800 01fff800 01fff800 01fff800
	 *   0x0f000030: 00003800 00003800 00000000 00000000
	 *   0x0f000040: 00010000 00000000 00000000 00000000
	 * The 0x0f000400 SDRAMU window surveys as zero, so leave it zeroed
	 * but make it stateful so WinCE can observe its own writes.
	 */
	vr41xx_seed_latch32(d->dmaau_regs, 0x00, 0x01fff800);
	vr41xx_seed_latch32(d->dmaau_regs, 0x04, 0x01fff800);
	vr41xx_seed_latch32(d->dmaau_regs, 0x08, 0x01fff800);
	vr41xx_seed_latch32(d->dmaau_regs, 0x0c, 0x01fff800);
	vr41xx_seed_latch32(d->dmaau_regs, 0x10, 0x00003800);
	vr41xx_seed_latch32(d->dmaau_regs, 0x14, 0x00003800);
	vr41xx_seed_latch32(d->dcu_regs, 0x00, 0x00010000);

	/*  TODO: VRC4173 has the KIU at offset 0x100?  */
	d->kiu_offset = 0x180;
	d->kiu_console_handle = -1;

	/*  Connect to the KIU and GIU interrupts:  */
	snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i",
	    machine->path, machine->bootstrap_cpu, VRIP_INTR_GIU);
	INTERRUPT_CONNECT(tmps, d->giu_irq);
	snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i",
	    machine->path, machine->bootstrap_cpu, VRIP_INTR_KIU);
	INTERRUPT_CONNECT(tmps, d->kiu_irq);  

	switch (cpumodel) {
	case 4101:
	case 4102:
	case 4111:
	case 4121:
		baseaddr = 0xb000000;
		break;
	case 4181:
		baseaddr = 0xa000000;
		dev_ram_init(machine, 0xb000000, 0x1000000, DEV_RAM_MIRROR,
		    0xa000000, NULL);
		break;
	case 4122:
	case 4131:
		baseaddr = 0xf000000;
		break;
	default:
		printf("Unimplemented VR cpu model\n");
		exit(1);
	}

	if (d->cpumodel == 4121 || d->cpumodel == 4181)
		snprintf(tmps, sizeof(tmps), "%s.cpu[%i].3",
		    machine->path, machine->bootstrap_cpu);
	else
		snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i",
		    machine->path, machine->bootstrap_cpu, VRIP_INTR_ETIMER);
	INTERRUPT_CONNECT(tmps, d->timer_irq);
	/*
	 * WinCE routes the VR4131 RTCL1 source through the dedicated timer
	 * CPU interrupt path rather than the generic SYSINT1 dispatcher.
	 * Feeding it back into VRIP bit 2 leaves SYSINT1 stuck at 0x0004
	 * while the kernel's primary poller explicitly masks that bit out.
	 */
	snprintf(tmps, sizeof(tmps), "%s.cpu[%i].3",
	    machine->path, machine->bootstrap_cpu);
	INTERRUPT_CONNECT(tmps, d->rtcl1_irq);

	memory_device_register(mem, "vr41xx", baseaddr, DEV_VR41XX_LENGTH,
	    dev_vr41xx_access, (void *)d, DM_DEFAULT, NULL);

	/*
	 *  TODO: Find out which controllers are at which addresses on
	 *  which chips.
	 */
	if (cpumodel == 4131) {
		snprintf(tmps, sizeof(tmps), "ns16550 irq=%s.cpu[%i].vrip.%i "
		    "addr=0x%" PRIx64" in_use=0 name2=siu", machine->path,
		    machine->bootstrap_cpu, VRIP_INTR_SIU,
		    (uint64_t) (baseaddr+0x800));
		device_add(machine, tmps);
	} else {
		/*  This is used by Linux and NetBSD:  */
		snprintf(tmps, sizeof(tmps), "ns16550 irq=%s.cpu[%i]."
		    "vrip.%i addr=0x%x name2=serial", machine->path,
		    machine->bootstrap_cpu, VRIP_INTR_SIU, 0xc000000);
		device_add(machine, tmps);
	}

	/*  Hm... maybe this should not be here.  TODO  */
	snprintf(tmps, sizeof(tmps), "pcic irq=%s.cpu[%i].vrip.%i addr="
	    "0x140003e0", machine->path, machine->bootstrap_cpu,
	    VRIP_INTR_GIU);
	device_add(machine, tmps);

	machine_add_tickfunction(machine, dev_vr41xx_tick, d,
	    DEV_VR41XX_TICKSHIFT);

	/*  Some machines (?) use ISA space at 0x15000000 instead of
	    0x14000000, eg IBM WorkPad Z50.  */
	dev_ram_init(machine, 0x15000000, 0x1000000, DEV_RAM_MIRROR,
	    0x14000000, NULL);

	vr41xx_diag_register(machine, d);

	return d;
}
