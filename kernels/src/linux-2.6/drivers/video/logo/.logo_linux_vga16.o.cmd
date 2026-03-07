cmd_drivers/video/logo/logo_linux_vga16.o := mipsel-linux-gcc -Wp,-MD,drivers/video/logo/.logo_linux_vga16.o.d -nostdinc -iwithprefix include -D__KERNEL__ -Iinclude  -Wall -Wstrict-prototypes -Wno-trigraphs -fno-strict-aliasing -fno-common -I /home/jroark/src/linux4be/linux4be-new/linux/include/asm/gcc -G 0 -mno-abicalls -fno-pic -pipe  -finline-limit=100000 -mabi=32 -mcpu=r4100 -mips3 -Wa,-32 -Wa,-march=r4100 -Wa,-mips3 -Wa,--trap -Iinclude/asm-mips/mach-vr41xx -Iinclude/asm-mips/mach-generic  -O2 -fomit-frame-pointer     -DKBUILD_BASENAME=logo_linux_vga16 -DKBUILD_MODNAME=logo_linux_vga16 -c -o drivers/video/logo/logo_linux_vga16.o drivers/video/logo/logo_linux_vga16.c

deps_drivers/video/logo/logo_linux_vga16.o := \
  drivers/video/logo/logo_linux_vga16.c \
  include/linux/linux_logo.h \
  include/linux/init.h \
    $(wildcard include/config/modules.h) \
    $(wildcard include/config/hotplug.h) \
  include/linux/config.h \
    $(wildcard include/config/h.h) \
  include/linux/compiler.h \
  include/linux/compiler-gcc2.h \
  include/linux/compiler-gcc.h \

drivers/video/logo/logo_linux_vga16.o: $(deps_drivers/video/logo/logo_linux_vga16.o)

$(deps_drivers/video/logo/logo_linux_vga16.o):
