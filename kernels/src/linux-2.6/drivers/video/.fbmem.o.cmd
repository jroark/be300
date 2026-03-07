cmd_drivers/video/fbmem.o := mipsel-linux-gcc -Wp,-MD,drivers/video/.fbmem.o.d -nostdinc -iwithprefix include -D__KERNEL__ -Iinclude  -Wall -Wstrict-prototypes -Wno-trigraphs -fno-strict-aliasing -fno-common -I /home/jroark/src/linux4be/linux4be-new/linux/include/asm/gcc -G 0 -mno-abicalls -fno-pic -pipe  -finline-limit=100000 -mabi=32 -mcpu=r4100 -mips3 -Wa,-32 -Wa,-march=r4100 -Wa,-mips3 -Wa,--trap -Iinclude/asm-mips/mach-vr41xx -Iinclude/asm-mips/mach-generic  -O2 -fomit-frame-pointer     -DKBUILD_BASENAME=fbmem -DKBUILD_MODNAME=fbmem -c -o drivers/video/fbmem.o drivers/video/fbmem.c

deps_drivers/video/fbmem.o := \
  drivers/video/fbmem.c \
    $(wildcard include/config/kmod.h) \
    $(wildcard include/config/apus.h) \
    $(wildcard include/config/framebuffer/console.h) \
    $(wildcard include/config/fb/retinaz3.h) \
    $(wildcard include/config/fb/amiga.h) \
    $(wildcard include/config/fb/clps711x.h) \
    $(wildcard include/config/fb/cyber.h) \
    $(wildcard include/config/fb/cyber2000.h) \
    $(wildcard include/config/fb/pm2.h) \
    $(wildcard include/config/fb/pm3.h) \
    $(wildcard include/config/fb/cirrus.h) \
    $(wildcard include/config/fb/aty.h) \
    $(wildcard include/config/fb/matrox.h) \
    $(wildcard include/config/fb/aty128.h) \
    $(wildcard include/config/fb/neomagic.h) \
    $(wildcard include/config/fb/virge.h) \
    $(wildcard include/config/fb/riva.h) \
    $(wildcard include/config/fb/3dfx.h) \
    $(wildcard include/config/fb/radeon.h) \
    $(wildcard include/config/fb/radeon/old.h) \
    $(wildcard include/config/fb/control.h) \
    $(wildcard include/config/fb/platinum.h) \
    $(wildcard include/config/fb/valkyrie.h) \
    $(wildcard include/config/fb/ct65550.h) \
    $(wildcard include/config/fb/imstt.h) \
    $(wildcard include/config/fb/s3trio.h) \
    $(wildcard include/config/fb/fm2.h) \
    $(wildcard include/config/fb/sis.h) \
    $(wildcard include/config/fb/trident.h) \
    $(wildcard include/config/fb/i810.h) \
    $(wildcard include/config/fb/sti.h) \
    $(wildcard include/config/fb/ffb.h) \
    $(wildcard include/config/fb/cg6.h) \
    $(wildcard include/config/fb/cg3.h) \
    $(wildcard include/config/fb/bw2.h) \
    $(wildcard include/config/fb/cg14.h) \
    $(wildcard include/config/fb/p9100.h) \
    $(wildcard include/config/fb/tcx.h) \
    $(wildcard include/config/fb/leo.h) \
    $(wildcard include/config/fb/of.h) \
    $(wildcard include/config/fb/vesa.h) \
    $(wildcard include/config/fb/sgivw.h) \
    $(wildcard include/config/fb/gbe.h) \
    $(wildcard include/config/fb/acorn.h) \
    $(wildcard include/config/fb/atari.h) \
    $(wildcard include/config/fb/mac.h) \
    $(wildcard include/config/fb/hga.h) \
    $(wildcard include/config/fb/iga.h) \
    $(wildcard include/config/apollo.h) \
    $(wildcard include/config/fb/q40.h) \
    $(wildcard include/config/fb/tga.h) \
    $(wildcard include/config/fb/hp300.h) \
    $(wildcard include/config/fb/g364.h) \
    $(wildcard include/config/fb/sa1100.h) \
    $(wildcard include/config/fb/pxa.h) \
    $(wildcard include/config/fb/sun3.h) \
    $(wildcard include/config/fb/hit.h) \
    $(wildcard include/config/fb/tx3912.h) \
    $(wildcard include/config/fb/e1355.h) \
    $(wildcard include/config/fb/pvr2.h) \
    $(wildcard include/config/fb/pmag/aa.h) \
    $(wildcard include/config/fb/pmag/ba.h) \
    $(wildcard include/config/fb/pmagb/b.h) \
    $(wildcard include/config/fb/maxine.h) \
    $(wildcard include/config/fb/simple.h) \
    $(wildcard include/config/fb/au1100.h) \
    $(wildcard include/config/fb/voodoo1.h) \
    $(wildcard include/config/fb/kyro.h) \
    $(wildcard include/config/fb/68328.h) \
    $(wildcard include/config/fb/asiliant.h) \
    $(wildcard include/config/fb/vga16.h) \
    $(wildcard include/config/gsp/resolver.h) \
    $(wildcard include/config/fb/virtual.h) \
    $(wildcard include/config/logo.h) \
    $(wildcard include/config/sun3.h) \
    $(wildcard include/config/mmu.h) \
  include/linux/config.h \
    $(wildcard include/config/h.h) \
  include/linux/module.h \
    $(wildcard include/config/modules.h) \
    $(wildcard include/config/modversions.h) \
    $(wildcard include/config/module/unload.h) \
    $(wildcard include/config/kallsyms.h) \
  include/linux/sched.h \
    $(wildcard include/config/numa.h) \
    $(wildcard include/config/smp.h) \
    $(wildcard include/config/security.h) \
    $(wildcard include/config/preempt.h) \
  include/asm/param.h \
  include/asm-mips/mach-generic/param.h \
  include/linux/capability.h \
  include/linux/types.h \
    $(wildcard include/config/uid16.h) \
  include/linux/posix_types.h \
  include/linux/stddef.h \
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
  include/linux/spinlock.h \
    $(wildcard include/config/debug/spinlock.h) \
  include/linux/preempt.h \
  include/linux/linkage.h \
  include/asm/linkage.h \
  include/linux/thread_info.h \
  include/linux/bitops.h \
  include/asm/bitops.h \
    $(wildcard include/config/cpu/has/llsc.h) \
    $(wildcard include/config/mips32.h) \
  include/asm/byteorder.h \
  include/linux/byteorder/little_endian.h \
  include/linux/byteorder/swab.h \
  include/linux/byteorder/generic.h \
  include/asm/system.h \
    $(wildcard include/config/cpu/has/sync.h) \
    $(wildcard include/config/cpu/has/wb.h) \
    $(wildcard include/config/cpu/has/lldscd.h) \
  include/linux/kernel.h \
    $(wildcard include/config/debug/spinlock/sleep.h) \
  /usr/local/lib/gcc-lib/mipsel-linux/2.96-sdelinuxmips-040127/include/stdarg.h \
  include/asm/bug.h \
  include/asm/break.h \
  include/asm/addrspace.h \
    $(wildcard include/config/cpu/r4300.h) \
    $(wildcard include/config/cpu/r4x00.h) \
    $(wildcard include/config/cpu/r5000.h) \
    $(wildcard include/config/cpu/nevada.h) \
    $(wildcard include/config/cpu/mips64.h) \
    $(wildcard include/config/cpu/r8000.h) \
    $(wildcard include/config/cpu/r10000.h) \
  include/asm-mips/mach-generic/spaces.h \
    $(wildcard include/config/dma/noncoherent.h) \
  include/asm/ptrace.h \
  include/asm/isadep.h \
    $(wildcard include/config/cpu/r3000.h) \
    $(wildcard include/config/cpu/tx39xx.h) \
  include/asm/hazards.h \
    $(wildcard include/config/cpu/rm9000.h) \
    $(wildcard include/config/cpu/mipsr2.h) \
  include/asm/thread_info.h \
    $(wildcard include/config/page/size/4kb.h) \
    $(wildcard include/config/page/size/8kb.h) \
    $(wildcard include/config/page/size/16kb.h) \
    $(wildcard include/config/page/size/64kb.h) \
    $(wildcard include/config/debug/stack/usage.h) \
  include/asm/processor.h \
    $(wildcard include/config/sgi/ip27.h) \
    $(wildcard include/config/cpu/has/prefetch.h) \
  include/linux/cache.h \
  include/asm/cache.h \
    $(wildcard include/config/mips/l1/cache/shift.h) \
  include/linux/threads.h \
    $(wildcard include/config/nr/cpus.h) \
  include/asm/cachectl.h \
  include/asm/cpu.h \
  include/asm/mipsregs.h \
    $(wildcard include/config/cpu/vr41xx.h) \
  include/asm/prefetch.h \
  include/linux/stringify.h \
  include/linux/timex.h \
    $(wildcard include/config/time/interpolation.h) \
  include/asm/timex.h \
  include/asm-mips/mach-vr41xx/timex.h \
  include/linux/time.h \
  include/linux/seqlock.h \
  include/asm/div64.h \
  include/linux/jiffies.h \
  include/linux/rbtree.h \
  include/linux/cpumask.h \
    $(wildcard include/config/hotplug/cpu.h) \
  include/linux/bitmap.h \
  include/linux/string.h \
  include/asm/string.h \
  include/asm/semaphore.h \
  include/asm/atomic.h \
  include/linux/wait.h \
  include/linux/list.h \
  include/linux/prefetch.h \
  include/linux/rwsem.h \
    $(wildcard include/config/rwsem/generic/spinlock.h) \
  include/linux/rwsem-spinlock.h \
  include/asm/page.h \
    $(wildcard include/config/discontigmem.h) \
    $(wildcard include/config/limited/dma.h) \
  include/asm/mmu.h \
  include/linux/smp.h \
  include/linux/sem.h \
    $(wildcard include/config/sysvipc.h) \
  include/linux/ipc.h \
  include/asm/ipcbuf.h \
  include/asm/sembuf.h \
  include/linux/signal.h \
  include/asm/signal.h \
    $(wildcard include/config/binfmt/irix.h) \
  include/asm/sigcontext.h \
  include/asm/siginfo.h \
    $(wildcard include/config/compat.h) \
  include/asm-generic/siginfo.h \
  include/linux/securebits.h \
  include/linux/fs_struct.h \
  include/linux/completion.h \
  include/linux/pid.h \
  include/linux/percpu.h \
  include/linux/slab.h \
    $(wildcard include/config/.h) \
  include/linux/gfp.h \
  include/linux/mmzone.h \
    $(wildcard include/config/force/max/zoneorder.h) \
  include/linux/numa.h \
  include/linux/topology.h \
  include/asm/topology.h \
  include/asm-mips/mach-generic/topology.h \
  include/asm-generic/topology.h \
  include/linux/kmalloc_sizes.h \
    $(wildcard include/config/large/allocs.h) \
  include/asm/percpu.h \
  include/asm-generic/percpu.h \
  include/linux/param.h \
  include/linux/resource.h \
  include/asm/resource.h \
  include/linux/timer.h \
  include/linux/aio.h \
  include/linux/workqueue.h \
  include/linux/aio_abi.h \
  include/asm/current.h \
  include/linux/stat.h \
  include/asm/stat.h \
  include/linux/kmod.h \
    $(wildcard include/config/hotplug.h) \
  include/linux/errno.h \
  include/asm/errno.h \
  include/asm-generic/errno-base.h \
  include/linux/elf.h \
  include/asm/elf.h \
  include/linux/kobject.h \
  include/linux/sysfs.h \
    $(wildcard include/config/sysfs.h) \
  include/linux/moduleparam.h \
  include/linux/init.h \
  include/asm/local.h \
  include/asm/module.h \
  include/asm/uaccess.h \
  include/linux/smp_lock.h \
  include/linux/major.h \
  include/linux/mm.h \
    $(wildcard include/config/stack/growsup.h) \
    $(wildcard include/config/hugetlb/page.h) \
    $(wildcard include/config/debug/pagealloc.h) \
    $(wildcard include/config/arch/gate/area.h) \
  include/linux/prio_tree.h \
  include/linux/fs.h \
    $(wildcard include/config/quota.h) \
    $(wildcard include/config/epoll.h) \
    $(wildcard include/config/auditsyscall.h) \
  include/linux/limits.h \
  include/linux/kdev_t.h \
  include/linux/ioctl.h \
  include/asm/ioctl.h \
  include/linux/dcache.h \
  include/linux/rcupdate.h \
  include/linux/radix-tree.h \
  include/linux/audit.h \
    $(wildcard include/config/audit.h) \
  include/linux/quota.h \
  include/linux/dqblk_xfs.h \
  include/linux/dqblk_v1.h \
  include/linux/dqblk_v2.h \
  include/linux/nfs_fs_i.h \
  include/linux/nfs.h \
  include/linux/sunrpc/msg_prot.h \
  include/linux/fcntl.h \
  include/asm/fcntl.h \
  include/linux/err.h \
  include/asm/pgtable.h \
  include/asm/pgtable-32.h \
  include/asm/fixmap.h \
  include/asm/pgtable-bits.h \
    $(wildcard include/config/cpu/sb1.h) \
    $(wildcard include/config/mips/uncached.h) \
  include/asm-generic/pgtable.h \
  include/linux/page-flags.h \
    $(wildcard include/config/swap.h) \
  include/linux/mman.h \
  include/asm/mman.h \
  include/linux/tty.h \
    $(wildcard include/config/legacy/pty/count.h) \
  include/linux/termios.h \
  include/asm/termios.h \
  include/asm/termbits.h \
  include/asm/ioctls.h \
  include/linux/tty_driver.h \
  include/linux/cdev.h \
  include/linux/tty_ldisc.h \
  include/linux/linux_logo.h \
  include/linux/proc_fs.h \
    $(wildcard include/config/proc/fs.h) \
    $(wildcard include/config/proc/devicetree.h) \
  include/linux/console.h \
  include/linux/devfs_fs_kernel.h \
    $(wildcard include/config/devfs/fs.h) \
  include/linux/device.h \
  include/linux/ioport.h \
  include/linux/pm.h \
    $(wildcard include/config/pm.h) \
  include/asm/io.h \
    $(wildcard include/config/swap/io/space.h) \
    $(wildcard include/config/sgi/ip22.h) \
  include/asm/cpu-features.h \
  include/asm-mips/mach-generic/cpu-feature-overrides.h \
  include/asm-mips/mach-generic/mangle-port.h \
  include/linux/fb.h \
  include/linux/notifier.h \
  drivers/video/console/fbcon.h \
  include/linux/vt_buffer.h \
    $(wildcard include/config/vga/console.h) \
    $(wildcard include/config/mda/console.h) \
  include/linux/vt_kern.h \
  include/linux/vt.h \
  include/linux/kd.h \
  include/linux/console_struct.h \

drivers/video/fbmem.o: $(deps_drivers/video/fbmem.o)

$(deps_drivers/video/fbmem.o):
