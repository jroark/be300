cmd_drivers/video/console/font_8x16.o := mipsel-linux-gcc -Wp,-MD,drivers/video/console/.font_8x16.o.d -nostdinc -iwithprefix include -D__KERNEL__ -Iinclude  -Wall -Wstrict-prototypes -Wno-trigraphs -fno-strict-aliasing -fno-common -I /home/jroark/src/linux4be/linux4be-new/linux/include/asm/gcc -G 0 -mno-abicalls -fno-pic -pipe  -finline-limit=100000 -mabi=32 -mcpu=r4100 -mips3 -Wa,-32 -Wa,-march=r4100 -Wa,-mips3 -Wa,--trap -Iinclude/asm-mips/mach-vr41xx -Iinclude/asm-mips/mach-generic  -O2 -fomit-frame-pointer     -DKBUILD_BASENAME=font_8x16 -DKBUILD_MODNAME=font -c -o drivers/video/console/font_8x16.o drivers/video/console/font_8x16.c

deps_drivers/video/console/font_8x16.o := \
  drivers/video/console/font_8x16.c \
  include/linux/font.h \
  include/linux/types.h \
    $(wildcard include/config/uid16.h) \
  include/linux/config.h \
    $(wildcard include/config/h.h) \
  include/linux/posix_types.h \
  include/linux/stddef.h \
  include/asm/posix_types.h \
  include/asm/sgidefs.h \
  include/asm/types.h \
    $(wildcard include/config/highmem.h) \
    $(wildcard include/config/64bit/phys/addr.h) \
    $(wildcard include/config/mips64.h) \
    $(wildcard include/config/lbd.h) \

drivers/video/console/font_8x16.o: $(deps_drivers/video/console/font_8x16.o)

$(deps_drivers/video/console/font_8x16.o):
