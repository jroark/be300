OUTPUT_ARCH(mips)
ENTRY(_start)

SECTIONS
{
  .tlbvec 0x80000000 : AT(0x00000000) {
    KEEP(*(.tlbvec))
  }

  .excvec 0x80000180 : AT(0x00000180) {
    KEEP(*(.excvec))
  }

  . = 0x80001000;
  .text : AT(ADDR(.text) - 0x80000000) {
    *(.text*)
    *(.rodata*)
  }

  . = ALIGN(16);
  .data : AT(ADDR(.data) - 0x80000000) {
    *(.data*)
    *(.sdata*)
  }

  . = ALIGN(16);
  __bss_start = .;
  .bss : AT(ADDR(.bss) - 0x80000000) {
    *(.sbss*)
    *(.bss*)
    *(COMMON)
  }
  __bss_end = .;

  _stack_top = 0x80008000;
}
