cmd_libbb/ptr_to_globals.o := mipsel-linux-gnu-gcc -Wp,-MD,libbb/.ptr_to_globals.o.d  -std=gnu99 -Iinclude -Ilibbb  -include include/autoconf.h -D_GNU_SOURCE -DNDEBUG -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -D"BB_VER=KBUILD_STR(1.24.2)" -DBB_BT=AUTOCONF_TIMESTAMP -Wall -Wshadow -Wwrite-strings -Wundef -Wstrict-prototypes -Wunused -Wunused-parameter -Wunused-function -Wunused-value -Wmissing-prototypes -Wmissing-declarations -Wno-format-security -Wdeclaration-after-statement -Wold-style-definition -fno-builtin-strlen -finline-limit=0 -fomit-frame-pointer -ffunction-sections -fdata-sections -fno-guess-branch-probability -funsigned-char -static-libgcc -falign-functions=1 -falign-jumps=1 -falign-labels=1 -falign-loops=1 -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-builtin-printf -Os -march=mips2    -D"KBUILD_STR(s)=#s" -D"KBUILD_BASENAME=KBUILD_STR(ptr_to_globals)"  -D"KBUILD_MODNAME=KBUILD_STR(ptr_to_globals)" -c -o libbb/ptr_to_globals.o libbb/ptr_to_globals.c

deps_libbb/ptr_to_globals.o := \
  libbb/ptr_to_globals.c \
  /usr/mipsel-linux-gnu/include/stdc-predef.h \
  /usr/mipsel-linux-gnu/include/errno.h \
  /usr/mipsel-linux-gnu/include/features.h \
  /usr/mipsel-linux-gnu/include/features-time64.h \
  /usr/mipsel-linux-gnu/include/bits/wordsize.h \
  /usr/mipsel-linux-gnu/include/sgidefs.h \
  /usr/mipsel-linux-gnu/include/bits/timesize.h \
  /usr/mipsel-linux-gnu/include/sys/cdefs.h \
  /usr/mipsel-linux-gnu/include/bits/long-double.h \
  /usr/mipsel-linux-gnu/include/gnu/stubs.h \
  /usr/mipsel-linux-gnu/include/gnu/stubs-o32_hard.h \
  /usr/mipsel-linux-gnu/include/bits/errno.h \
  /usr/mipsel-linux-gnu/include/linux/errno.h \
  /usr/mipsel-linux-gnu/include/asm/errno.h \
  /usr/mipsel-linux-gnu/include/asm-generic/errno-base.h \
  /usr/mipsel-linux-gnu/include/bits/types/error_t.h \

libbb/ptr_to_globals.o: $(deps_libbb/ptr_to_globals.o)

$(deps_libbb/ptr_to_globals.o):
