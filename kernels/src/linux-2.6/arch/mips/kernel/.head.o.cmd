cmd_arch/mips/kernel/head.o := mipsel-linux-gcc -Wp,-MD,arch/mips/kernel/.head.o.d -nostdinc -iwithprefix include -D__KERNEL__ -Iinclude  -D__ASSEMBLY__ -I /home/jroark/src/linux4be/linux4be-new/linux/include/asm/gcc -G 0 -mno-abicalls -fno-pic -pipe  -finline-limit=100000 -mabi=32 -mcpu=r4100 -mips3 -Wa,-32 -Wa,-march=r4100 -Wa,-mips3 -Wa,--trap -Iinclude/asm-mips/mach-vr41xx -Iinclude/asm-mips/mach-generic  -Wall -Wstrict-prototypes -Wno-trigraphs -fno-strict-aliasing -fno-common -I /home/jroark/src/linux4be/linux4be-new/linux/include/asm/gcc -G 0 -mno-abicalls -fno-pic -pipe  -finline-limit=100000 -mabi=32 -mcpu=r4100 -mips3 -Wa,-32 -Wa,-march=r4100 -Wa,-mips3 -Wa,--trap -Iinclude/asm-mips/mach-vr41xx -Iinclude/asm-mips/mach-generic  -O2 -fomit-frame-pointer  -march=r4100   -c -o arch/mips/kernel/head.o arch/mips/kernel/head.S

deps_arch/mips/kernel/head.o := \
  arch/mips/kernel/head.S \
    $(wildcard include/config/sgi/ip27.h) \
    $(wildcard include/config/arc64.h) \
    $(wildcard include/config/mapped/kernel.h) \
    $(wildcard include/config/mips64.h) \
    $(wildcard include/config/smp.h) \
  include/linux/config.h \
    $(wildcard include/config/h.h) \
  include/linux/init.h \
    $(wildcard include/config/modules.h) \
    $(wildcard include/config/hotplug.h) \
  include/linux/compiler.h \
  include/linux/threads.h \
    $(wildcard include/config/nr/cpus.h) \
  include/asm/asm.h \
    $(wildcard include/config/cpu/has/prefetch.h) \
  include/asm/sgidefs.h \
  include/asm/regdef.h \
  include/asm/page.h \
    $(wildcard include/config/page/size/4kb.h) \
    $(wildcard include/config/page/size/8kb.h) \
    $(wildcard include/config/page/size/16kb.h) \
    $(wildcard include/config/page/size/64kb.h) \
    $(wildcard include/config/64bit/phys/addr.h) \
    $(wildcard include/config/discontigmem.h) \
    $(wildcard include/config/limited/dma.h) \
  include/asm-mips/mach-generic/spaces.h \
    $(wildcard include/config/mips32.h) \
    $(wildcard include/config/dma/noncoherent.h) \
  include/asm/mipsregs.h \
    $(wildcard include/config/cpu/vr41xx.h) \
  include/linux/linkage.h \
  include/asm/linkage.h \
  include/asm/hazards.h \
    $(wildcard include/config/cpu/rm9000.h) \
    $(wildcard include/config/cpu/mipsr2.h) \
    $(wildcard include/config/cpu/r10000.h) \
  include/asm/stackframe.h \
    $(wildcard include/config/cpu/r3000.h) \
    $(wildcard include/config/cpu/tx39xx.h) \
  include/asm/offset.h \

arch/mips/kernel/head.o: $(deps_arch/mips/kernel/head.o)

$(deps_arch/mips/kernel/head.o):
