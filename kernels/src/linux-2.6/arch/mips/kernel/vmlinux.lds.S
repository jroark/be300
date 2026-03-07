

















OUTPUT_ARCH(mips)
ENTRY(kernel_entry)
jiffies = jiffies_64;
SECTIONS
{

  . = 0x80001000;
  /* read-only */

  _text = .;			/* Text and read-only data */

  .text : {
    *(.text)
    __sched_text_start = .; *(.sched.text) __sched_text_end = .;
    *(.fixup)
    *(.gnu.warning)
  } =0

  _etext = .;			/* End of text section */


  . = ALIGN(16);		/* Exception table */

  __start___ex_table = .;
  __ex_table : { *(__ex_table) }
  __stop___ex_table = .;

  __start___dbe_table = .;	/* Exception table for data bus errors */

  __dbe_table : { *(__dbe_table) }
  __stop___dbe_table = .;

   .rodata : AT(ADDR(.rodata) - 0) { *(.rodata) *(.rodata.*) *(__vermagic) } .rodata1 : AT(ADDR(.rodata1) - 0) { *(.rodata1) } __ksymtab : AT(ADDR(__ksymtab) - 0) { __start___ksymtab = .; *(__ksymtab) __stop___ksymtab = .; } __ksymtab_gpl : AT(ADDR(__ksymtab_gpl) - 0) { __start___ksymtab_gpl = .; *(__ksymtab_gpl) __stop___ksymtab_gpl = .; } __kcrctab : AT(ADDR(__kcrctab) - 0) { __start___kcrctab = .; *(__kcrctab) __stop___kcrctab = .; } __kcrctab_gpl : AT(ADDR(__kcrctab_gpl) - 0) { __start___kcrctab_gpl = .; *(__kcrctab_gpl) __stop___kcrctab_gpl = .; } __ksymtab_strings : AT(ADDR(__ksymtab_strings) - 0) { *(__ksymtab_strings) }

  /* writeable */

  .data : {			/* Data */

    . = . + 0;	/* for CONFIG_MAPPED_KERNEL */

    *(.data.init_task)

    *(.data)

   /* Align the initial ramdisk image (INITRD) on page boundaries. */

   . = ALIGN(4096);
   __rd_start = .;
   *(.initrd)
   . = ALIGN(4096);
   __rd_end = .;

    CONSTRUCTORS
  }
  _gp = . + 0x8000;
  .lit8 : { *(.lit8) }
  .lit4 : { *(.lit4) }
  /* We want the small data sections together, so single-instruction offsets
     can access them all, and initialized data all before uninitialized, so
     we can shorten the on-disk segment size.  */

  .sdata     : { *(.sdata) }

  . = ALIGN(4096);
  __nosave_begin = .;
  .data_nosave : { *(.data.nosave) }
  . = ALIGN(4096);
  __nosave_end = .;

  . = ALIGN(32);
  .data.cacheline_aligned : { *(.data.cacheline_aligned) }

  _edata =  .;			/* End of data section */


  /* will be freed after init */

  . = ALIGN(4096);		/* Init code and data */

  __init_begin = .;
  /* /DISCARD/ doesn't work for .reginfo */

  .reginfo : { *(.reginfo) }
  .init.text : {
	_sinittext = .;
	*(.init.text)
	_einittext = .;
  }
  .init.data : { *(.init.data) }
  . = ALIGN(16);
  __setup_start = .;
  .init.setup : { *(.init.setup) }
  __setup_end = .;
  __start___param = .;
  __param : { *(__param) }
  __stop___param = .;

  .early_initcall.init : {
  __earlyinitcall_start = .;
	*(.initcall.early1.init)
  }
  __earlyinitcall_end = .;

  __initcall_start = .;
  .initcall.init : {
	*(.initcall1.init)
	*(.initcall2.init)
	*(.initcall3.init)
	*(.initcall4.init)
	*(.initcall5.init)
	*(.initcall6.init)
	*(.initcall7.init)
  }
  __initcall_end = .;

  __con_initcall_start = .;
  .con_initcall.init : { *(.con_initcall.init) }
  __con_initcall_end = .;
   .security_initcall.init : { __security_initcall_start = .; *(.security_initcall.init) __security_initcall_end = .; }
  . = ALIGN(4096);
  __initramfs_start = .;
  .init.ramfs : { *(.init.ramfs) }
  __initramfs_end = .;
  . = ALIGN(32);
  __per_cpu_start = .;
  .data.percpu  : { *(.data.percpu) }
  __per_cpu_end = .;
  . = ALIGN(4096);
  __init_end = .;
  /* freed after init ends here */


  __bss_start = .;		/* BSS */

  .sbss      : {
    *(.sbss)
    *(.scommon)
  }
  .bss : {
    *(.bss)
    *(COMMON)
  }
  __bss_stop = .;

  _end = . ;

  /* Sections to be discarded */

  /DISCARD/ : {
        *(.exit.text)
        *(.exit.data)
        *(.exitcall.exit)
  }

  /* This is the MIPS specific mdebug section.  */

  .mdebug : { *(.mdebug) }
  /* These are needed for ELF backends which have not yet been
     converted to the new style linker.  */

  .stab 0 : { *(.stab) }
  .stabstr 0 : { *(.stabstr) }
  /* DWARF debug sections.
     Symbols in the .debug DWARF section are relative to the beginning of the
     section so we begin .debug at 0.  It's not clear yet what needs to happen
     for the others.   */

  .debug          0 : { *(.debug) }
  .debug_srcinfo  0 : { *(.debug_srcinfo) }
  .debug_aranges  0 : { *(.debug_aranges) }
  .debug_pubnames 0 : { *(.debug_pubnames) }
  .debug_sfnames  0 : { *(.debug_sfnames) }
  .line           0 : { *(.line) }
  /* These must appear regardless of  .  */

  .gptab.sdata : { *(.gptab.data) *(.gptab.sdata) }
  .gptab.sbss : { *(.gptab.bss) *(.gptab.sbss) }
  .comment : { *(.comment) }
  .note : { *(.note) }
}
