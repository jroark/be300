cmd_arch/mips/vr41xx/common/int-handler.o := mipsel-linux-gcc -Wp,-MD,arch/mips/vr41xx/common/.int-handler.o.d -nostdinc -iwithprefix include -D__KERNEL__ -Iinclude  -D__ASSEMBLY__ -I /home/jroark/src/linux4be/linux4be-new/linux/include/asm/gcc -G 0 -mno-abicalls -fno-pic -pipe  -finline-limit=100000 -mabi=32 -mcpu=r4100 -mips3 -Wa,-32 -Wa,-march=r4100 -Wa,-mips3 -Wa,--trap -Iinclude/asm-mips/mach-vr41xx -Iinclude/asm-mips/mach-generic  -Wall -Wstrict-prototypes -Wno-trigraphs -fno-strict-aliasing -fno-common -I /home/jroark/src/linux4be/linux4be-new/linux/include/asm/gcc -G 0 -mno-abicalls -fno-pic -pipe  -finline-limit=100000 -mabi=32 -mcpu=r4100 -mips3 -Wa,-32 -Wa,-march=r4100 -Wa,-mips3 -Wa,--trap -Iinclude/asm-mips/mach-vr41xx -Iinclude/asm-mips/mach-generic  -O2 -fomit-frame-pointer    -c -o arch/mips/vr41xx/common/int-handler.o arch/mips/vr41xx/common/int-handler.S

deps_arch/mips/vr41xx/common/int-handler.o := \
  arch/mips/vr41xx/common/int-handler.S \
  include/asm/asm.h \
    $(wildcard include/config/cpu/has/prefetch.h) \
  include/linux/config.h \
    $(wildcard include/config/h.h) \
  include/asm/sgidefs.h \
  include/asm/regdef.h \
  include/asm/mipsregs.h \
    $(wildcard include/config/cpu/vr41xx.h) \
    $(wildcard include/config/page/size/4kb.h) \
    $(wildcard include/config/page/size/16kb.h) \
    $(wildcard include/config/page/size/64kb.h) \
  include/linux/linkage.h \
  include/asm/linkage.h \
  include/asm/hazards.h \
    $(wildcard include/config/cpu/rm9000.h) \
    $(wildcard include/config/cpu/mipsr2.h) \
    $(wildcard include/config/cpu/r10000.h) \
  include/asm/stackframe.h \
    $(wildcard include/config/mips32.h) \
    $(wildcard include/config/smp.h) \
    $(wildcard include/config/mips64.h) \
    $(wildcard include/config/cpu/r3000.h) \
    $(wildcard include/config/cpu/tx39xx.h) \
  include/linux/threads.h \
    $(wildcard include/config/nr/cpus.h) \
  include/asm/offset.h \

arch/mips/vr41xx/common/int-handler.o: $(deps_arch/mips/vr41xx/common/int-handler.o)

$(deps_arch/mips/vr41xx/common/int-handler.o):
