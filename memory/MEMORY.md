# BE-300 Emulator — Agent Memory

## Project
Casio BE-300 (NEC VR4131 MIPS LE) emulator in Unicorn Engine.
Working kernel: `linux4be20040908/vmlinux` (2.6.8.1)
All work on branch: `claude/explain-codebase-mm1561dhacl5ikyh-zdk3b`

## Key Files
- `src/machine.c` — main emulator logic (~3700 lines), all hooks
- `src/machine.h` — machine_t struct definition
- `src/bus.c` — memory/MMIO mapping
- `src/ui.c` — SDL2 framebuffer display

## Build/Run
- **Build**: in Docker `mips-dev` container: `cd /work && mkdir -p build-docker && cd build-docker && cmake .. && make -j$(nproc) be300`
- **Run**: `timeout 90s ./be300 --kernel ../linux4be20040908/vmlinux > /tmp/out.txt 2> /tmp/err.txt`
- **Commit/push**: on host (not in container)

## Current Boot Status (2026-03-02)
See `HANDOFF.md` for detailed current state.
