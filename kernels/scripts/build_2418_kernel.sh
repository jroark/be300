#!/bin/bash
# Build Linux 2.4.18 for Casio BE-300 (NEC VR4131)
# Run inside the mips-dev Docker container: docker compose run --rm mips-dev bash /work/kernels/scripts/build_2418_kernel.sh
set -e

WORK=/work
BUILDDIR=$WORK/kernels/build/linux-2.4.18-be300
SRCOVERLAY=$WORK/kernels/src/linux-2.4.18
TARBALL=$WORK/kernels/build/linux-2.4.18.tar.gz

echo "=== Step 1: Download vanilla Linux 2.4.18 ==="
mkdir -p $WORK/kernels/build
cd $WORK/kernels/build
if [ ! -f "$TARBALL" ]; then
    wget -q https://cdn.kernel.org/pub/linux/kernel/v2.4/linux-2.4.18.tar.gz
fi

echo "=== Step 2: Extract and overlay ==="
rm -rf "$BUILDDIR"
tar xzf linux-2.4.18.tar.gz
mv linux "$BUILDDIR"

# Overlay linux-mips.org BE-300 arch + includes
cp -r $SRCOVERLAY/arch/mips/* $BUILDDIR/arch/mips/
cp -r $SRCOVERLAY/include/asm-mips/* $BUILDDIR/include/asm-mips/

# Copy BE-300 drivers
cp $SRCOVERLAY/drivers/video/sfb.c $BUILDDIR/drivers/video/
cp $SRCOVERLAY/drivers/char/serial_be300.c $BUILDDIR/drivers/char/
cp $SRCOVERLAY/drivers/char/serial_be300.h $BUILDDIR/drivers/char/
cp $SRCOVERLAY/drivers/char/scan_be300.c $BUILDDIR/drivers/char/

# Overwrite modified vanilla files
cp $SRCOVERLAY/drivers/char/tty_io.c $BUILDDIR/drivers/char/
cp $SRCOVERLAY/drivers/char/Config.in $BUILDDIR/drivers/char/

cd "$BUILDDIR"

echo "=== Step 3: Apply GCC 12+ compatibility patches ==="

# Replace __FUNCTION__ with __func__ (GCC 3.4+ deprecation)
find . \( -name "*.c" -o -name "*.h" \) -not -path "./scripts/*" | \
  xargs sed -i 's/__FUNCTION__/__func__/g'

# Fix -mcpu (deprecated) -> -march in arch Makefile, and fix MIPS ISA conflict
sed -i 's/-mcpu=r4600 -mips2/-march=mips2/' arch/mips/Makefile
sed -i 's/-mcpu=/-march=/g' arch/mips/Makefile

# Fix comma-operator lvalue in UP __IRQ_STAT macro
sed -i 's/#define __IRQ_STAT(cpu, member)\t((void)(cpu), irq_stat\[0\].member)/#define __IRQ_STAT(cpu, member)\t(irq_stat[0].member)/' include/linux/irq_cpustat.h

# Fix impossible constraint "=h" in delay.h (multu hi/lo)
sed -i '/"multu/,/);/{
  s/"=h" (usecs), "=l" (lo)/"=r" (usecs), "=r" (lo)/
  s/"multu\\t%2,%3"/"multu\\t%2,%3\\n\\tmfhi\\t%0\\n\\tmflo\\t%1"/
}' include/asm-mips/delay.h

# Fix =h/=l in div64.h
sed -i '/: "Jr" (__high), "Jr" (base)/,/: "hi", "lo");/{
  s/: "hi", "lo");/);/
}' include/asm-mips/div64.h

# Fix =h/=l in time.c
sed -i '/"multu  %2,%3"/,/: "r" (count), "r" (quotient));/{
  s/: "=l" (tmp), "=h" (res)/: "=r" (tmp), "=r" (res)/
  s/"multu  %2,%3"/"multu  %2,%3\\n\\tmflo\\t%0\\n\\tmfhi\\t%1"/
}' arch/mips/kernel/time.c

# Fix const-lvalue in uaccess.h get_user macros
sed -i 's/__get_user_nocheck((__typeof__(*(ptr)))(x)/__get_user_nocheck((x)/' include/asm-mips/uaccess.h
sed -i 's/__get_user_check((__typeof__(*(ptr)))(x)/__get_user_check((x)/' include/asm-mips/uaccess.h
# Use unsigned long for __gu_val to avoid const issues
sed -i 's/^__typeof__(*(ptr)) __gu_val;/unsigned long __gu_val;/' include/asm-mips/uaccess.h
sed -i 's/^__typeof(*(ptr)) __gu_val;/unsigned long __gu_val;/' include/asm-mips/uaccess.h
# Remove empty asm initializers
sed -i '/__asm__("":"=r" (__gu_val));/d' include/asm-mips/uaccess.h
sed -i '/__asm__("":"=r" (__gu_err));/d' include/asm-mips/uaccess.h
# Initialize vars directly
sed -i 's/^long __gu_err;/long __gu_err = 0;/' include/asm-mips/uaccess.h

# Fix const-lvalue in unaligned.h
sed -i 's/__typeof__(*(ptr)) __val;/unsigned long long __val;/' include/asm-mips/unaligned.h
sed -i 's/^\t__val;/\t(__typeof__(*(ptr)))__val;/' include/asm-mips/unaligned.h

# Fix cast-on-lvalue in readdir.c
sed -i 's/((char \*) dirent) += reclen;/dirent = (void *)((char *) dirent + reclen);/g' fs/readdir.c

# Fix cast-on-lvalue in read_write.c
sed -i 's/(u32)tot_len += len;/tot_len += (unsigned int)len;/' fs/read_write.c

# Fix __FUNCTION__ string concat in system.h die macros
sed -i 's/__FILE__ ":"__func__/__FILE__/' include/asm-mips/system.h

# Fix __FUNCTION__ concat in super.c
sed -i 's/panic(__func__ ": unable to allocate root device");/panic("%s: unable to allocate root device", __func__);/' fs/super.c

# Fix __FUNCTION__ concat in file.c
sed -i 's/printk (KERN_ERR __func__ "array/printk (KERN_ERR "%s: array/' fs/file.c
sed -i 's/"\\n", num);/"\\n", __func__, num);/' fs/file.c

# Fix cast-on-lvalue in fbcon.c
sed -i 's/\*--(u32 \*)p/*(u32 *)(p -= 4)/' drivers/video/fbcon.c

# Fix missing PER_LINUX include
sed -i '/#include <linux\/sched.h>/a #include <linux\/personality.h>' arch/mips/kernel/process.c

# Fix sfb.c missing header
sed -i 's/#include <asm\/vr41xx-platdep.h>/\/* vr41xx-platdep.h not needed - constants hardcoded below *\//' drivers/video/sfb.c

# Add set_io_port_base to io.h
sed -i '/^extern unsigned long mips_io_port_base;/a\
\nstatic inline void set_io_port_base(unsigned long base)\n{\n\tmips_io_port_base = base;\n}' include/asm-mips/io.h

# Add GCC compat flags and -fno-stack-protector to Makefile
sed -i 's/-fomit-frame-pointer -fno-strict-aliasing -fno-common/-fomit-frame-pointer -fno-strict-aliasing -fno-common \\\n\t  -fgnu89-inline -Wno-error -w -fno-stack-protector/' Makefile

echo "=== Step 4: Create missing VR41xx common code ==="

# Create vr4122 header stub
mkdir -p include/asm-mips/vr4122
cat > include/asm-mips/vr4122/vr4122.h << 'EOF'
#ifndef __ASM_VR4122_H
#define __ASM_VR4122_H
/* Minimal stub - VR4131 is superset of VR4122 */
#endif
EOF

# Create VR41xx common code (BCU, CMU, PMU, timer, IRQ, prom stubs)
cp $WORK/kernels/build/linux-2.4.18-be300/arch/mips/vr41xx/vr4122/common/vr41xx_common.c \
   arch/mips/vr41xx/vr4122/common/vr41xx_common.c 2>/dev/null || true
# (This file should already exist from the overlay step if running fresh;
#  on fresh runs it needs to be created - see the plan for full contents)

echo "=== Step 5: Create board Makefiles ==="

cat > arch/mips/vr41xx/vr4131/casio-be300/Makefile << 'ENDMK'
O_TARGET = be300.o
obj-y = setup.o
obj-$(CONFIG_BLK_DEV_IDE) += ide-be300.o
include $(TOPDIR)/Rules.make
ENDMK

cat > arch/mips/vr41xx/vr4122/common/Makefile << 'ENDMK'
O_TARGET = vr4122_common.o
obj-y = vr41xx_common.o
include $(TOPDIR)/Rules.make
ENDMK

echo "=== Step 6: Update build system ==="

# Add BE-300 to arch/mips/Makefile (if not already present)
if ! grep -q "CONFIG_CASIO_BE300" arch/mips/Makefile; then
    sed -i "/^ifdef CONFIG_MIPS_PB1000/i\\
#\\
# Casio Cassiopeia BE-300\\
#\\
ifdef CONFIG_CASIO_BE300\\
CORE_FILES    += arch/mips/vr41xx/vr4131/casio-be300/be300.o arch/mips/vr41xx/vr4122/common/vr4122_common.o\\
SUBDIRS       += arch/mips/vr41xx/vr4131/casio-be300 arch/mips/vr41xx/vr4122/common\\
LOADADDR      += 0x80001000\\
ifdef CONFIG_EMBEDDED_RAMDISK\\
CORE_FILES    += arch/mips/ramdisk/ramdisk.o\\
endif\\
endif\\
" arch/mips/Makefile
fi

# Add serial_be300 and scan_be300 to drivers/char/Makefile
sed -i '/obj-$(CONFIG_SERIAL_SA1100)/a\
obj-$(CONFIG_SERIAL_BE300) += serial_be300.o\
obj-$(CONFIG_CASIO_BE300) += scan_be300.o' drivers/char/Makefile

# Disable default SERIAL for BE300
sed -i '/^ifdef CONFIG_DECSTATION/i\
ifdef CONFIG_CASIO_BE300\
  SERIAL   =\
endif' drivers/char/Makefile

# Add sfb to drivers/video/Makefile
sed -i '/obj-$(CONFIG_FB_VIRTUAL)/a\
obj-$(CONFIG_FB_SIMPLE)           += sfb.o' drivers/video/Makefile

# Add sfb to fbmem.c init table
sed -i '/extern int sstfb_setup/a\
extern int sfb_init(void);\
extern int sfb_setup(char*);' drivers/video/fbmem.c
sed -i '/#ifdef CONFIG_FB_VIRTUAL/i\
#ifdef CONFIG_FB_SIMPLE\
\t{ "sfb", sfb_init, sfb_setup },\
#endif' drivers/video/fbmem.c

# Add CONFIG_FB_SIMPLE to drivers/video/Config.in
sed -i "/tristate.*Virtual Frame Buffer/i\\
   if [ \"\\\$CONFIG_CASIO_BE300\" = \"y\" ]; then\\
      bool '  Simple frame buffer' CONFIG_FB_SIMPLE\\
      if [ \"\\\$CONFIG_FB_SIMPLE\" = \"y\" ]; then\\
         hex '    Video RAM base address' CONFIG_FB_SIMPLE_MEMBASE aa200000\\
         hex '    Video RAM size (0=auto)' CONFIG_FB_SIMPLE_MEMSIZE 0\\
      fi\\
   fi" drivers/video/Config.in

echo "=== Step 7: Configure ==="
cp $SRCOVERLAY/autobuild.config .config
yes "" | make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- oldconfig 2>&1 | tail -5
make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- dep 2>&1 | tail -3

echo "=== Step 8: Prepare embedded ramdisk ==="
mkdir -p arch/mips/ramdisk
cat > arch/mips/ramdisk/ld.script << 'EOF'
OUTPUT_FORMAT("elf32-tradlittlemips")
OUTPUT_ARCH(mips)
SECTIONS
{
  .initrd :
  {
    *(.data)
  }
}
EOF
cp $WORK/kernels/ramdisk-pgui-full.gz arch/mips/ramdisk/ramdisk.gz
cd arch/mips/ramdisk
mipsel-linux-gnu-ld -r -T ld.script -b binary -o ramdisk.o ramdisk.gz
cd "$BUILDDIR"

echo "=== Step 9: Build ==="
make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- vmlinux -j$(nproc)

echo "=== Step 10: Verify ==="
ls -lh vmlinux
file vmlinux
mipsel-linux-gnu-nm vmlinux | grep "__rd_start\|__rd_end"
strings vmlinux | grep "Linux version"
echo "=== BUILD COMPLETE ==="
