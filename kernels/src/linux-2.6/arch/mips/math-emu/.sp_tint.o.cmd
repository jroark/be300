cmd_arch/mips/math-emu/sp_tint.o := mipsel-linux-gcc -Wp,-MD,arch/mips/math-emu/.sp_tint.o.d -nostdinc -iwithprefix include -D__KERNEL__ -Iinclude  -Wall -Wstrict-prototypes -Wno-trigraphs -fno-strict-aliasing -fno-common -I /home/jroark/src/linux4be/linux4be-new/linux/include/asm/gcc -G 0 -mno-abicalls -fno-pic -pipe  -finline-limit=100000 -mabi=32 -mcpu=r4100 -mips3 -Wa,-32 -Wa,-march=r4100 -Wa,-mips3 -Wa,--trap -Iinclude/asm-mips/mach-vr41xx -Iinclude/asm-mips/mach-generic  -O2 -fomit-frame-pointer     -DKBUILD_BASENAME=sp_tint -DKBUILD_MODNAME=sp_tint -c -o arch/mips/math-emu/sp_tint.o arch/mips/math-emu/sp_tint.c

deps_arch/mips/math-emu/sp_tint.o := \
  arch/mips/math-emu/sp_tint.c \
  include/linux/kernel.h \
    $(wildcard include/config/debug/spinlock/sleep.h) \
  /usr/local/lib/gcc-lib/mipsel-linux/2.96-sdelinuxmips-040127/include/stdarg.h \
  include/linux/linkage.h \
  include/linux/config.h \
    $(wildcard include/config/h.h) \
  include/asm/linkage.h \
  include/linux/stddef.h \
  include/linux/types.h \
    $(wildcard include/config/uid16.h) \
  include/linux/posix_types.h \
  include/asm/posix_types.h \
  include/asm/sgidefs.h \
  include/asm/types.h \
    $(wildcard include/config/highmem.h) \
    $(wildcard include/config/64bit/phys/addr.h) \
    $(wildcard include/config/mips64.h) \
    $(wildcard include/config/lbd.h) \
  include/linux/compiler.h \
  include/linux/compiler-gcc2.h \
  include/linux/compiler-gcc.h \
  include/asm/byteorder.h \
  include/linux/byteorder/little_endian.h \
  include/linux/byteorder/swab.h \
  include/linux/byteorder/generic.h \
  include/asm/bug.h \
  include/asm/break.h \
  arch/mips/math-emu/ieee754sp.h \
  arch/mips/math-emu/ieee754int.h \
  arch/mips/math-emu/ieee754.h \

arch/mips/math-emu/sp_tint.o: $(deps_arch/mips/math-emu/sp_tint.o)

$(deps_arch/mips/math-emu/sp_tint.o):
