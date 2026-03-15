cmd_arch/mips/vr41xx/common/bcu.o := mipsel-linux-gcc -Wp,-MD,arch/mips/vr41xx/common/.bcu.o.d -nostdinc -iwithprefix include -D__KERNEL__ -Iinclude  -Wall -Wstrict-prototypes -Wno-trigraphs -fno-strict-aliasing -fno-common -I /home/jroark/src/linux4be/linux4be-new/linux/include/asm/gcc -G 0 -mno-abicalls -fno-pic -pipe  -finline-limit=100000 -mabi=32 -mcpu=r4100 -mips3 -Wa,-32 -Wa,-march=r4100 -Wa,-mips3 -Wa,--trap -Iinclude/asm-mips/mach-vr41xx -Iinclude/asm-mips/mach-generic  -O2 -fomit-frame-pointer     -DKBUILD_BASENAME=bcu -DKBUILD_MODNAME=bcu -c -o arch/mips/vr41xx/common/bcu.o arch/mips/vr41xx/common/bcu.c

deps_arch/mips/vr41xx/common/bcu.o := \
  arch/mips/vr41xx/common/bcu.c \
  include/linux/init.h \
    $(wildcard include/config/modules.h) \
    $(wildcard include/config/hotplug.h) \
  include/linux/config.h \
    $(wildcard include/config/h.h) \
  include/linux/compiler.h \
  include/linux/compiler-gcc2.h \
  include/linux/compiler-gcc.h \
  include/linux/ioport.h \
  include/linux/smp.h \
    $(wildcard include/config/smp.h) \
  include/linux/types.h \
    $(wildcard include/config/uid16.h) \
  include/linux/posix_types.h \
  include/linux/stddef.h \
  include/asm/posix_types.h \
  include/asm/sgidefs.h \
  include/asm/types.h \
    $(wildcard include/config/highmem.h) \
    $(wildcard include/config/64bit/phys/addr.h) \
    $(wildcard include/config/mips64.h) \
    $(wildcard include/config/lbd.h) \
  include/asm/cpu.h \
  include/asm/io.h \
    $(wildcard include/config/swap/io/space.h) \
    $(wildcard include/config/sgi/ip22.h) \
    $(wildcard include/config/mips32.h) \
    $(wildcard include/config/dma/noncoherent.h) \
  include/asm/addrspace.h \
    $(wildcard include/config/cpu/r4300.h) \
    $(wildcard include/config/cpu/r4x00.h) \
    $(wildcard include/config/cpu/r5000.h) \
    $(wildcard include/config/cpu/nevada.h) \
    $(wildcard include/config/cpu/mips64.h) \
    $(wildcard include/config/cpu/r8000.h) \
    $(wildcard include/config/cpu/r10000.h) \
  include/asm-mips/mach-generic/spaces.h \
  include/asm/cpu-features.h \
  include/asm-mips/mach-generic/cpu-feature-overrides.h \
  include/asm/page.h \
    $(wildcard include/config/page/size/4kb.h) \
    $(wildcard include/config/page/size/8kb.h) \
    $(wildcard include/config/page/size/16kb.h) \
    $(wildcard include/config/page/size/64kb.h) \
    $(wildcard include/config/discontigmem.h) \
    $(wildcard include/config/limited/dma.h) \
  include/asm/pgtable-bits.h \
    $(wildcard include/config/cpu/r3000.h) \
    $(wildcard include/config/cpu/tx39xx.h) \
    $(wildcard include/config/cpu/sb1.h) \
    $(wildcard include/config/cpu/rm9000.h) \
    $(wildcard include/config/mips/uncached.h) \
  include/asm/processor.h \
    $(wildcard include/config/sgi/ip27.h) \
    $(wildcard include/config/cpu/has/prefetch.h) \
  include/linux/cache.h \
  include/linux/kernel.h \
    $(wildcard include/config/debug/spinlock/sleep.h) \
  /usr/local/lib/gcc-lib/mipsel-linux/2.96-sdelinuxmips-040127/include/stdarg.h \
  include/linux/linkage.h \
  include/asm/linkage.h \
  include/asm/byteorder.h \
  include/linux/byteorder/little_endian.h \
  include/linux/byteorder/swab.h \
  include/linux/byteorder/generic.h \
  include/asm/bug.h \
  include/asm/break.h \
  include/asm/cache.h \
    $(wildcard include/config/mips/l1/cache/shift.h) \
  include/linux/threads.h \
    $(wildcard include/config/nr/cpus.h) \
  include/asm/cachectl.h \
  include/asm/mipsregs.h \
    $(wildcard include/config/cpu/vr41xx.h) \
  include/asm/hazards.h \
    $(wildcard include/config/cpu/mipsr2.h) \
  include/asm/prefetch.h \
  include/asm/system.h \
    $(wildcard include/config/cpu/has/sync.h) \
    $(wildcard include/config/cpu/has/wb.h) \
    $(wildcard include/config/cpu/has/llsc.h) \
    $(wildcard include/config/cpu/has/lldscd.h) \
  include/asm/ptrace.h \
  include/asm/isadep.h \
  include/asm-mips/mach-generic/mangle-port.h \

arch/mips/vr41xx/common/bcu.o: $(deps_arch/mips/vr41xx/common/bcu.o)

$(deps_arch/mips/vr41xx/common/bcu.o):
