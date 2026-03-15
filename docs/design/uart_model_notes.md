# BE-300 UART Modeling Notes

Preflight checklist (from `serial_be300.c`):
- Confirm register offset stride (`BE300_REG_SHIFT`) and effective MMIO offsets.
- Confirm read/write width (8-bit accesses on wider-spaced registers).
- Confirm byte-lane behavior expectations on little-endian CPU.
- Confirm IRQ behavior and ack semantics for IRQ 19 path.
- Confirm console init defaults (9600, ttyS0 mapping).

Decision gate:
- If behavior matches standard 8250 mapping closely enough, test reuse path.
- If not, implement custom SysBus UART to exactly match BE300 register semantics.
