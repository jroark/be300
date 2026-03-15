cmd_scripts/mod/empty.o := mipsel-linux-gcc -Wp,-MD,scripts/mod/.empty.o.d -nostdinc -iwithprefix include -D__KERNEL__ -Iinclude  -Wall -Wstrict-prototypes -Wno-trigraphs -fno-strict-aliasing -fno-common -I /home/jroark/src/linux4be/linux4be-new/linux/include/asm/gcc -G 0 -mno-abicalls -fno-pic -pipe  -finline-limit=100000 -mabi=32 -mcpu=r4100 -mips3 -Wa,-32 -Wa,-march=r4100 -Wa,-mips3 -Wa,--trap -Iinclude/asm-mips/mach-vr41xx -Iinclude/asm-mips/mach-generic  -O2 -fomit-frame-pointer     -DKBUILD_BASENAME=empty -DKBUILD_MODNAME=empty -c -o scripts/mod/empty.o scripts/mod/empty.c

deps_scripts/mod/empty.o := \
  scripts/mod/empty.c \

scripts/mod/empty.o: $(deps_scripts/mod/empty.o)

$(deps_scripts/mod/empty.o):
