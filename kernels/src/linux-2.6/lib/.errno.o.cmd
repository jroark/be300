cmd_lib/errno.o := mipsel-linux-gcc -Wp,-MD,lib/.errno.o.d -nostdinc -iwithprefix include -D__KERNEL__ -Iinclude  -Wall -Wstrict-prototypes -Wno-trigraphs -fno-strict-aliasing -fno-common -I /home/jroark/src/linux4be/linux4be-new/linux/include/asm/gcc -G 0 -mno-abicalls -fno-pic -pipe  -finline-limit=100000 -mabi=32 -mcpu=r4100 -mips3 -Wa,-32 -Wa,-march=r4100 -Wa,-mips3 -Wa,--trap -Iinclude/asm-mips/mach-vr41xx -Iinclude/asm-mips/mach-generic  -O2 -fomit-frame-pointer     -DKBUILD_BASENAME=errno -DKBUILD_MODNAME=errno -c -o lib/errno.o lib/errno.c

deps_lib/errno.o := \
  lib/errno.c \

lib/errno.o: $(deps_lib/errno.o)

$(deps_lib/errno.o):
