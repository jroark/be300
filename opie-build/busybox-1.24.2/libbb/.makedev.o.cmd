cmd_libbb/makedev.o := mipsel-linux-gnu-gcc -Wp,-MD,libbb/.makedev.o.d  -std=gnu99 -Iinclude -Ilibbb  -include include/autoconf.h -D_GNU_SOURCE -DNDEBUG -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -D"BB_VER=KBUILD_STR(1.24.2)" -DBB_BT=AUTOCONF_TIMESTAMP -Wall -Wshadow -Wwrite-strings -Wundef -Wstrict-prototypes -Wunused -Wunused-parameter -Wunused-function -Wunused-value -Wmissing-prototypes -Wmissing-declarations -Wno-format-security -Wdeclaration-after-statement -Wold-style-definition -fno-builtin-strlen -finline-limit=0 -fomit-frame-pointer -ffunction-sections -fdata-sections -fno-guess-branch-probability -funsigned-char -static-libgcc -falign-functions=1 -falign-jumps=1 -falign-labels=1 -falign-loops=1 -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-builtin-printf -Os -march=mips2    -D"KBUILD_STR(s)=#s" -D"KBUILD_BASENAME=KBUILD_STR(makedev)"  -D"KBUILD_MODNAME=KBUILD_STR(makedev)" -c -o libbb/makedev.o libbb/makedev.c

deps_libbb/makedev.o := \
  libbb/makedev.c \
  /usr/mipsel-linux-gnu/include/stdc-predef.h \
  include/platform.h \
    $(wildcard include/config/werror.h) \
    $(wildcard include/config/big/endian.h) \
    $(wildcard include/config/little/endian.h) \
    $(wildcard include/config/nommu.h) \
  /usr/lib/gcc-cross/mipsel-linux-gnu/12/include/limits.h \
  /usr/lib/gcc-cross/mipsel-linux-gnu/12/include/syslimits.h \
  /usr/mipsel-linux-gnu/include/limits.h \
  /usr/mipsel-linux-gnu/include/bits/libc-header-start.h \
  /usr/mipsel-linux-gnu/include/features.h \
  /usr/mipsel-linux-gnu/include/features-time64.h \
  /usr/mipsel-linux-gnu/include/bits/wordsize.h \
  /usr/mipsel-linux-gnu/include/sgidefs.h \
  /usr/mipsel-linux-gnu/include/bits/timesize.h \
  /usr/mipsel-linux-gnu/include/sys/cdefs.h \
  /usr/mipsel-linux-gnu/include/bits/long-double.h \
  /usr/mipsel-linux-gnu/include/gnu/stubs.h \
  /usr/mipsel-linux-gnu/include/gnu/stubs-o32_hard.h \
  /usr/mipsel-linux-gnu/include/bits/posix1_lim.h \
  /usr/mipsel-linux-gnu/include/bits/local_lim.h \
  /usr/mipsel-linux-gnu/include/linux/limits.h \
  /usr/mipsel-linux-gnu/include/bits/pthread_stack_min-dynamic.h \
  /usr/mipsel-linux-gnu/include/bits/posix2_lim.h \
  /usr/mipsel-linux-gnu/include/bits/xopen_lim.h \
  /usr/mipsel-linux-gnu/include/bits/uio_lim.h \
  /usr/mipsel-linux-gnu/include/byteswap.h \
  /usr/mipsel-linux-gnu/include/bits/byteswap.h \
  /usr/mipsel-linux-gnu/include/bits/types.h \
  /usr/mipsel-linux-gnu/include/bits/typesizes.h \
  /usr/mipsel-linux-gnu/include/bits/time64.h \
  /usr/mipsel-linux-gnu/include/endian.h \
  /usr/mipsel-linux-gnu/include/bits/endian.h \
  /usr/mipsel-linux-gnu/include/bits/endianness.h \
  /usr/mipsel-linux-gnu/include/bits/uintn-identity.h \
  /usr/lib/gcc-cross/mipsel-linux-gnu/12/include/stdint.h \
  /usr/mipsel-linux-gnu/include/stdint.h \
  /usr/mipsel-linux-gnu/include/bits/wchar.h \
  /usr/mipsel-linux-gnu/include/bits/stdint-intn.h \
  /usr/mipsel-linux-gnu/include/bits/stdint-uintn.h \
  /usr/mipsel-linux-gnu/include/bits/stdint-least.h \
  /usr/lib/gcc-cross/mipsel-linux-gnu/12/include/stdbool.h \
  /usr/mipsel-linux-gnu/include/unistd.h \
  /usr/mipsel-linux-gnu/include/bits/posix_opt.h \
  /usr/mipsel-linux-gnu/include/bits/environments.h \
  /usr/lib/gcc-cross/mipsel-linux-gnu/12/include/stddef.h \
  /usr/mipsel-linux-gnu/include/bits/confname.h \
  /usr/mipsel-linux-gnu/include/bits/getopt_posix.h \
  /usr/mipsel-linux-gnu/include/bits/getopt_core.h \
  /usr/mipsel-linux-gnu/include/bits/unistd.h \
  /usr/mipsel-linux-gnu/include/bits/unistd-decl.h \
  /usr/mipsel-linux-gnu/include/bits/unistd_ext.h \
  /usr/mipsel-linux-gnu/include/linux/close_range.h \
  /usr/mipsel-linux-gnu/include/sys/sysmacros.h \
  /usr/mipsel-linux-gnu/include/bits/sysmacros.h \

libbb/makedev.o: $(deps_libbb/makedev.o)

$(deps_libbb/makedev.o):
