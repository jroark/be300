# HANDOFF POST PASS 42 — launcher dependency-check decoded; welcome.exe likely stuck on a dependency

**Date**: 2026-04-23 (late evening)
**Branch**: `investigate/pass38-gwes`
**Stall state unchanged**: "Starting" OAL splash.

## §1 Summary

Pass 42 decoded the kernel's `launcher_dependencies_satisfied_for_init_entry` function (PC `0x800805A8`), which revealed exactly how HKLM\init\Launch entries gate on their `Depend<NN>` dependencies. Runtime probe data from Pass 38 shows only **3 of 5 Launch entries ever signal "I'm ready"** — meaning any entry depending on the non-signaling ones (Boot.exe or coshell on first boot) is parked indefinitely. If welcome.exe is a Launch entry with a Depend chain pointing at those, we have our root cause.

HKLM\Services scan: clean (only repllog.exe has `Services\` strings, unrelated to welcome). CeCompressROM cracking: still uncracked; relative-offset and 5 absolute-offset variants all fail.

## §2 `launcher_dependencies_satisfied_for_init_entry` decoded

```c
// Located via mcp__ghidra__search_functions_by_name "launcher_depend"
// Full decomp via mcp__ghidra__decompile_function_by_address 0x800805A8

undefined4 launcher_dependencies_satisfied_for_init_entry(int entry_idx) {
    // Each entry is 0x94 u32s = 0x250 bytes (matches Pass 32 launcher stride)
    // Deps start at entry_base + 0x08 (=2 u32s in), terminated by u16 0
    
    uint16_t dep_id = entry->deps[0];
    ushort* dep_ptr = &entry->deps[0];
    
    while (true) {
        if (dep_id == 0) return 1;  // No more deps → satisfied
        
        // Linear search the launcher entries for this dep ID
        int found_idx = 0;
        for (int i = 0; i < g_launch_entry_count; i++) {
            if (dep_id == entries[i].order_id) {
                found_idx = i;
                break;
            }
        }
        
        if (found_idx == g_launch_entry_count) {
            // Dependency ID not in the launcher list — skip (treat as satisfied)
            dep_id = dep_ptr[1];  // next
        } else {
            if (entries[found_idx].started == 0) {
                return 0;  // NOT satisfied — waiting for that entry to spawn+signal
            }
            dep_id = dep_ptr[1];  // next
        }
        dep_ptr++;
    }
}
```

The critical field is `entries[i].started` at offset `+0x04` in the 0x250-byte struct. **It is set to 1 ONLY when the corresponding launch process successfully spawns AND signals back "I'm ready" via the coredll thunk**.

## §3 Runtime evidence: only 3 / 5 Launch entries signal ready

Extracted from `build-host/pass38_gwes_stderr.log`:

```
[BE300_LIFECYCLE_SUMMARY] launcher_module_ready_notify hits=3 pc=0x80080d38
```

Three ready-signal calls in 60 s. All three have `ra=0x01f8f3e0` = inside coredll's `SignalStarted`-equivalent thunk — so the signals come from USER-MODE processes via the coredll API.

Pass 32 counted 5 HKLM\init\Launch entries (`0x250`-stride table at user VA `0x0203B4D0`). Five spawns happened in Pass 38, but only THREE signal "I'm ready":

| Launch entry | Spawned in 60 s? | Signaled ready? |
|---|---|---|
| shell.exe (order ~20) | YES (hit #3) | likely YES |
| device.exe (order ~30) | YES (hit #4) | likely YES |
| gwes.exe (order ~50) | YES (hit #5) | likely YES |
| Boot.exe (order ~55) | YES (hit #6) | **NO** — Boot.exe calls `FUN_1310C(0x0101003c)` (REBOOT) before ever signaling ready |
| coshell.exe (order ~80?) | **NO (only on warm reset)** | **NO** on first cold boot |

So on our current 60-second cold-boot run, **2 of the 5 launcher slots remain `started=0` forever**. Any Launch entry whose Depend list points at Boot or coshell will loop forever in `WaitForMultipleObjects(_DAT_8066AF00)` and never reach its `CreateProcessW`.

## §4 Strongest hypothesis: welcome.exe is a Launch entry with Depend = coshell (or Boot)

Without cracking default.fdf, we can't read `Launch<N>=welcome.exe` / `Depend<N>=<ids>` directly. But the elimination cascade now looks like:

- welcome.exe's path string is NOT hardcoded anywhere in the ROM (Pass 41).
- welcome.exe MUST be spawned via `FUN_8008690c` (there is only one CreateProcessW kernel thunk), and it takes its image name string from the registry (only path without a hardcoded string match).
- The ONLY kernel-side CreateProcessW caller reading strings from the registry is `FUN_800808c4` at PC `0x80080C80..0x80080CA0` (Pass 39).
- Therefore welcome.exe IS a Launch entry in HKLM\init. It never spawns because of a dependency.
- The only launcher entries that never have their `started` flag set to 1 in our emulator are Boot (which reboots) and coshell (which doesn't spawn on first boot).

This is **falsifiable**: once default.fdf is decompressed, we should see a `Launch<N>=welcome.exe` key with `Depend<N>` containing coshell's launch ID (and possibly others).

## §5 HKLM\Services scan: clean

UTF-16LE scan of NK + all 95 XIP modules for `Services\`, `HKLM\Services`, `AutoStart`, `AutoLoad`, `OnStart`, `StartMode`, `OnBoot`:

- NK has 3 hits for `"Services\"` at VA `0x80277136..0x8027767C`. These are inside **`repllog.exe`** (TOC entry 11, loaded into NK VA ~`0x80277000+`) — ActiveSync replication paths. Not related to welcome.
- `11_repllog.exe.bin` has the same 3 hits (confirms they're repllog-local).
- Zero other modules reference services/autostart.

So no second HKLM\Services-based spawn mechanism exists on BE-300. The HKLM\init\Launch enumerator at `FUN_800808c4` is the ONLY kernel-side spawn gate.

## §6 CeCompressROM crack attempts this pass: still uncracked

New variants tried:
- Relative-offset LZ77 (distance-back from current output, no ring buffer): all 4 bitfield splits (12+4, 11+5, 10+6) with min_match 1/2/3 → 0/20 pages exact. Failures were early `offset > output` errors because the ring starts empty and relative back-refs can't reach data.
- Confirmed the format is NOT a simple relative-offset LZ77.

The 12+4 mm=2 variant (from Pass 40) remains closest: page 19 at 324/326 bytes, with `"Launch"` substrings present in the partial output. The off-by-one length suggests the true format uses `mlen = (packed >> 12) + 2` for ONLY some matches, or there's a length-extension byte for long matches.

Next-session approach for Pass 43 Q1 (crack CeCompressROM):
- Port from a DIFFERENT reference: Microsoft's Windows Embedded Compact source (from the 2020 leak) has a `compchain.c` decoder. The MSFT-internal `CECompressChain` format is documented.
- Or: search for `mszip` / `cecompressrom` / `LZ77_CE` on github for alternative implementations beyond KodaSec's XIP-specific one.
- Or: hand-trace page 19 (smallest) by decoding the bitstream manually, one token at a time, against the known next-value `"Launch"`. The expected output after `Launch` must be a digit `'0'..'9' (UTF-16LE) = XX 00`. If my decoder produces `36 6e` instead of `XX 00`, the correct interpretation should produce exactly `XX 00`. Work backwards from that constraint to identify the right bit layout.

## §7 Pass 43 ranked priorities

1. **P1** — crack CeCompressROM via hand-tracing page 19 against known-expected `Launch<NN>` = digit pattern constraint. Highest-signal, most tractable.
2. **P2** — in `be300_probe.c`, add a memwatch on `entries[*].started` (offsets `+0x04`, `+0x254`, `+0x4a4`, `+0x6F4`, `+0x944` in the `0x250`-stride table at user VA `0x0203B4D0`). Capture which ENTRIES have `started=1` vs `started=0` over a 60-s run; the `started=0` entries are the candidates welcome depends on.
3. **P3** — instrument `launcher_dependencies_satisfied_for_init_entry` entry and its `return 0` paths (two inside the function). Dump the `dep_id` at each `return 0`. This directly names which dependency is blocking which entry.
4. **P4** — force coshell to spawn on first boot (test if it breaks welcome's dependency loop). Dangerous (real-HW-accuracy violation, per CLAUDE.md "no guest patches") but would confirm/refute the hypothesis cheaply.

## §8 Deliverables

- This handoff doc.
- No probe code changes.
- Memory update: `project_pass42_dependency_chain_decoded.md`.
- Branch `investigate/pass38-gwes` only.
