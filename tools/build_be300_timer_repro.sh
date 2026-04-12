#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC_DIR="$ROOT/tools/be300_timer_repro"
OUT_DIR="$ROOT/build-host/be300_timer_repro"

mkdir -p "$OUT_DIR"

docker compose run --rm mips-dev /bin/bash -lc "
set -euo pipefail
cd /work
mkdir -p /work/build-host/be300_timer_repro

mipsel-linux-gnu-gcc \
    -EL -mips32 -G0 -mno-abicalls -fno-pic -nostdlib -ffreestanding \
    -Wl,-T,/work/tools/be300_timer_repro/be300_timer_repro.ld \
    -Wl,--build-id=none \
    -Wl,-e,_start \
    -DREPRO_TIMER=0 \
    -o /work/build-host/be300_timer_repro/be300_timer_repro_control.elf \
    /work/tools/be300_timer_repro/be300_timer_repro.S

mipsel-linux-gnu-objdump -d \
    /work/build-host/be300_timer_repro/be300_timer_repro_control.elf \
    > /work/build-host/be300_timer_repro/be300_timer_repro_control.disasm.txt

mipsel-linux-gnu-gcc \
    -EL -mips32 -G0 -mno-abicalls -fno-pic -nostdlib -ffreestanding \
    -Wl,-T,/work/tools/be300_timer_repro/be300_timer_repro.ld \
    -Wl,--build-id=none \
    -Wl,-e,_start \
    -DREPRO_TIMER=1 \
    -o /work/build-host/be300_timer_repro/be300_timer_repro_timer.elf \
    /work/tools/be300_timer_repro/be300_timer_repro.S

mipsel-linux-gnu-objdump -d \
    /work/build-host/be300_timer_repro/be300_timer_repro_timer.elf \
    > /work/build-host/be300_timer_repro/be300_timer_repro_timer.disasm.txt

mipsel-linux-gnu-gcc \
    -EL -mips32 -G0 -mno-abicalls -fno-pic -nostdlib -ffreestanding \
    -Wl,-T,/work/tools/be300_timer_repro/be300_timer_repro.ld \
    -Wl,--build-id=none \
    -Wl,-e,_start \
    -DREPRO_TIMER=1 \
    -DREPRO_IRQ_PROBE=1 \
    -o /work/build-host/be300_timer_repro/be300_timer_repro_irq_probe.elf \
    /work/tools/be300_timer_repro/be300_timer_repro.S

mipsel-linux-gnu-objdump -d \
    /work/build-host/be300_timer_repro/be300_timer_repro_irq_probe.elf \
    > /work/build-host/be300_timer_repro/be300_timer_repro_irq_probe.disasm.txt

echo built /work/build-host/be300_timer_repro/be300_timer_repro_control.elf
echo built /work/build-host/be300_timer_repro/be300_timer_repro_timer.elf
echo built /work/build-host/be300_timer_repro/be300_timer_repro_irq_probe.elf
"
