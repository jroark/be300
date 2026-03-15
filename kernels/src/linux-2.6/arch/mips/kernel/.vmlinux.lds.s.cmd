cmd_arch/mips/kernel/vmlinux.lds.s := mipsel-linux-gcc -E -Wp,-MD,arch/mips/kernel/.vmlinux.lds.s.d -nostdinc -iwithprefix include -D__KERNEL__ -Iinclude  -D__ASSEMBLY__ -I /home/jroark/src/linux4be/linux4be-new/linux/include/asm/gcc -G 0 -mno-abicalls -fno-pic -pipe  -finline-limit=100000 -mabi=32 -mcpu=r4100 -mips3 -Wa,-32 -Wa,-march=r4100 -Wa,-mips3 -Wa,--trap -Iinclude/asm-mips/mach-vr41xx -Iinclude/asm-mips/mach-generic  -Wall -Wstrict-prototypes -Wno-trigraphs -fno-strict-aliasing -fno-common -I /home/jroark/src/linux4be/linux4be-new/linux/include/asm/gcc -G 0 -mno-abicalls -fno-pic -pipe  -finline-limit=100000 -mabi=32 -mcpu=r4100 -mips3 -Wa,-32 -Wa,-march=r4100 -Wa,-mips3 -Wa,--trap -Iinclude/asm-mips/mach-vr41xx -Iinclude/asm-mips/mach-generic  -O2 -fomit-frame-pointer  -march=r4100 -D"LOADADDR=0x80001000" -D"JIFFIES=jiffies_64" -imacros /home/jroark/src/linux4be/linux4be-new/linux/include/asm-mips/sn/mapped_kernel.h -P -C -Umips    -o arch/mips/kernel/vmlinux.lds.s arch/mips/kernel/vmlinux.lds.S 

deps_arch/mips/kernel/vmlinux.lds.s := \
  arch/mips/kernel/vmlinux.lds.S \
    $(wildcard include/config/boot/elf64.h) \
    $(wildcard include/config/mapped/kernel.h) \
  include/linux/config.h \
    $(wildcard include/config/h.h) \
  include/asm/addrspace.h \
    $(wildcard include/config/cpu/r4300.h) \
    $(wildcard include/config/cpu/r4x00.h) \
    $(wildcard include/config/cpu/r5000.h) \
    $(wildcard include/config/cpu/nevada.h) \
    $(wildcard include/config/cpu/mips64.h) \
    $(wildcard include/config/cpu/r8000.h) \
    $(wildcard include/config/cpu/r10000.h) \
  include/asm-mips/mach-generic/spaces.h \
    $(wildcard include/config/mips32.h) \
    $(wildcard include/config/mips64.h) \
    $(wildcard include/config/dma/noncoherent.h) \
  include/asm-generic/vmlinux.lds.h \

arch/mips/kernel/vmlinux.lds.s: $(deps_arch/mips/kernel/vmlinux.lds.s)

$(deps_arch/mips/kernel/vmlinux.lds.s):
