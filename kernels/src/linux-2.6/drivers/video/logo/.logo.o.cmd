cmd_drivers/video/logo/logo.o := mipsel-linux-gcc -Wp,-MD,drivers/video/logo/.logo.o.d -nostdinc -iwithprefix include -D__KERNEL__ -Iinclude  -Wall -Wstrict-prototypes -Wno-trigraphs -fno-strict-aliasing -fno-common -I /home/jroark/src/linux4be/linux4be-new/linux/include/asm/gcc -G 0 -mno-abicalls -fno-pic -pipe  -finline-limit=100000 -mabi=32 -mcpu=r4100 -mips3 -Wa,-32 -Wa,-march=r4100 -Wa,-mips3 -Wa,--trap -Iinclude/asm-mips/mach-vr41xx -Iinclude/asm-mips/mach-generic  -O2 -fomit-frame-pointer     -DKBUILD_BASENAME=logo -DKBUILD_MODNAME=logo -c -o drivers/video/logo/logo.o drivers/video/logo/logo.c

deps_drivers/video/logo/logo.o := \
  drivers/video/logo/logo.c \
    $(wildcard include/config/m68k.h) \
    $(wildcard include/config/mips.h) \
    $(wildcard include/config/logo/linux/mono.h) \
    $(wildcard include/config/logo/superh/mono.h) \
    $(wildcard include/config/logo/linux/vga16.h) \
    $(wildcard include/config/logo/superh/vga16.h) \
    $(wildcard include/config/logo/linux/clut224.h) \
    $(wildcard include/config/logo/be300/clut224.h) \
    $(wildcard include/config/logo/dec/clut224.h) \
    $(wildcard include/config/logo/mac/clut224.h) \
    $(wildcard include/config/logo/parisc/clut224.h) \
    $(wildcard include/config/logo/sgi/clut224.h) \
    $(wildcard include/config/x86/visws.h) \
    $(wildcard include/config/logo/sun/clut224.h) \
    $(wildcard include/config/logo/superh/clut224.h) \
  include/linux/config.h \
    $(wildcard include/config/h.h) \
  include/linux/linux_logo.h \
  include/linux/init.h \
    $(wildcard include/config/modules.h) \
    $(wildcard include/config/hotplug.h) \
  include/linux/compiler.h \
  include/linux/compiler-gcc2.h \
  include/linux/compiler-gcc.h \
  include/linux/stddef.h \
  include/asm/bootinfo.h \
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
  include/asm/setup.h \

drivers/video/logo/logo.o: $(deps_drivers/video/logo/logo.o)

$(deps_drivers/video/logo/logo.o):
