/*
 *  src/devices/pcmcia_attr.c — PCMCIA card attribute-memory window.
 *
 *  Range: PA 0x0B400000-0x0B700000 (3 MB).
 *
 *  Split out of src/be300_devices.c. Public API (declared in be300.h):
 *    be300_register_pcmcia_attr_window.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"

#include "be300.h"
#include "devices.h"
#include "hw/cf.h"

#include "devices_internal.h"

/*
 *  PCMCIA card attribute-memory window at PA 0x0B400000-0x0B700000.
 *
 *  Background: pcmcia.dll has two card-window read helpers that walk
 *  attribute memory looking for CIS tuples:
 *    - 0x0198a3b0 / lbu @ 0x0198a3f0 = `pcmcia_window_read_byte(base,
 *      offset*2)` — flooded PA 0x0B600000+ when no card was attached.
 *    - 0x0198a488 / lbu @ 0x0198a490 = `simple_read_byte(base+offset)`
 *      — flooded PA 0x0B400000+0x1F5 once the 0x0B600000 stub shifted
 *      the scan to a second polling loop.
 *
 *  No-card behavior (preserved from Pass 48): real BE-300 with no card
 *  inserted reads the attribute-memory window's pulled-up bus as 0xFF.
 *  PC Card Standard Release 8 §3.2.10 defines `0xFF = CISTPL_END`, so a
 *  real CIS scanner reads one 0xFF byte and stops; without that fill,
 *  unmapped reads return 0 (CISTPL_NULL) and the parser walks forever.
 *
 *  Card-present behavior (added 2026-04-25 for `--cf`): when a CF image
 *  is attached, route reads inside the window into the seeded CIS held
 *  by `cf_state_t` so pcmcia.dll can identify the card by FUNCID/MANFID
 *  and dispatch to atadisk.dll instead of prompting the user for a
 *  driver name. PCMCIA attribute memory is byte-wide on every other
 *  byte (PC Card Standard Release 8 §4.7.4); `cf_cis_read` already
 *  encodes that stride.
 *
 *  Range: 0x0B400000-0x0B700000 (3 MB) covers both observed CIS-scan
 *  loops plus margin for any future PCMCIA bridge programmed elsewhere
 *  in the lower 0x0B0xxxxx-0x0BFxxxxx range. `be300_companion_ab_window`
 *  covers 0x0B000000-0x0B010000 separately (companion-chip secondary
 *  decode window — different semantics, must remain RAM-backed).
 *
 *  TODO(2026-04-25): the *complete* fix is to model the VRC4173 PCMCIA
 *  host bridge — its window-translation registers determine where the
 *  card window actually decodes. Without that, pcmcia.dll programs
 *  default windows that land at 0x0B4-0x0B6. This shim is good enough
 *  to expose the seeded CIS to the guest; revisit once the bridge is
 *  modeled.
 */
struct be300_pcmcia_attr_device {
    cf_state_t *cf;
};

DEVICE_ACCESS(be300_pcmcia_attr)
{
    struct be300_pcmcia_attr_device *d =
        (struct be300_pcmcia_attr_device *)extra;

    (void)cpu; (void)mem;
    if (writeflag != MEM_READ) {
        /* Writes are silently dropped (host-bus pull-up has no backing
         * and the seeded CIS is treated as ROM). */
        return 1;
    }

    if (d && cf_present(d->cf)) {
        /*
         * pcmcia.dll uses two helpers against this window:
         *   - PC 0x0198a490 simple_read_byte(base+N): direct byte-addressed
         *     CIS at base 0x0B400000.
         *   - PC 0x0198a3f0 pcmcia_window_read_byte(base, N*2): striped
         *     attribute-memory CIS at base 0x0B600000 (relative_addr
         *     here lands at 0x200000+).
         * Map both onto the seeded CIS image accordingly. PC Card
         * Standard Release 8 §4.7.4 documents the every-other-byte
         * stride in attribute-memory mode.
         */
        for (size_t i = 0; i < len; i++) {
            uint32_t off = (uint32_t)relative_addr + (uint32_t)i;
            uint32_t cis_idx;
            if (off >= 0x200000u) {
                uint32_t local = off - 0x200000u;
                cis_idx = (local & 1u) ? 0xFFFFFFFFu : (local >> 1);
            } else {
                cis_idx = off;
            }
            data[i] = cf_cis_read_byte(d->cf, cis_idx);
        }
    } else {
        memset(data, 0xFF, len);
    }
    return 1;
}

void be300_register_pcmcia_attr_window(struct machine *gxm, machine_t *m)
{
    struct be300_pcmcia_attr_device *d;

    CHECK_ALLOCATION(d = calloc(1, sizeof(*d)));
    d->cf = m ? &m->cf[BE300_PRIMARY_CF_SLOT] : NULL;

    /*
     * The CIS window is a read-only view of the card's attribute memory, but
     * the CF socket status settles after the guest reaches the FUNCID tuple.
     * Keep reads dispatched so that transition cannot be optimized away.
     */
    memory_device_register(gxm->memory, "be300_pcmcia_attr",
        0x0B400000ULL, 0x300000ULL,
        dev_be300_pcmcia_attr_access, (void *)d,
        DM_DEFAULT, NULL);
}
