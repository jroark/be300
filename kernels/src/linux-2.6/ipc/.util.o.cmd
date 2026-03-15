cmd_ipc/util.o := mipsel-linux-gcc -Wp,-MD,ipc/.util.o.d -nostdinc -iwithprefix include -D__KERNEL__ -Iinclude  -Wall -Wstrict-prototypes -Wno-trigraphs -fno-strict-aliasing -fno-common -I /home/jroark/src/linux4be/linux4be-new/linux/include/asm/gcc -G 0 -mno-abicalls -fno-pic -pipe  -finline-limit=100000 -mabi=32 -mcpu=r4100 -mips3 -Wa,-32 -Wa,-march=r4100 -Wa,-mips3 -Wa,--trap -Iinclude/asm-mips/mach-vr41xx -Iinclude/asm-mips/mach-generic  -O2 -fomit-frame-pointer     -DKBUILD_BASENAME=util -DKBUILD_MODNAME=util -c -o ipc/util.o ipc/util.c

deps_ipc/util.o := \
  ipc/util.c \
  include/linux/config.h \
    $(wildcard include/config/h.h) \
  include/linux/mm.h \
    $(wildcard include/config/discontigmem.h) \
    $(wildcard include/config/numa.h) \
    $(wildcard include/config/stack/growsup.h) \
    $(wildcard include/config/hugetlb/page.h) \
    $(wildcard include/config/highmem.h) \
    $(wildcard include/config/debug/pagealloc.h) \
    $(wildcard include/config/arch/gate/area.h) \
  include/linux/sched.h \
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
    $(wildcard include/config/mmu.h) \
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
  include/linux/errno.h \
  include/asm/errno.h \
  include/asm-generic/errno-base.h \
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
  include/linux/stat.h \
  include/asm/stat.h \
  include/linux/kobject.h \
  include/linux/sysfs.h \
    $(wildcard include/config/sysfs.h) \
  include/linux/radix-tree.h \
  include/linux/audit.h \
    $(wildcard include/config/audit.h) \
  include/linux/init.h \
    $(wildcard include/config/modules.h) \
    $(wildcard include/config/hotplug.h) \
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
  include/linux/shm.h \
  include/asm/shmparam.h \
  include/asm/shmbuf.h \
  include/linux/msg.h \
  include/asm/msgbuf.h \
    $(wildcard include/config/cpu/little/endian.h) \
  include/linux/smp_lock.h \
  include/linux/vmalloc.h \
  include/linux/highuid.h \
  include/linux/security.h \
    $(wildcard include/config/security/network.h) \
  include/linux/binfmts.h \
  include/linux/sysctl.h \
  include/linux/skbuff.h \
    $(wildcard include/config/netfilter.h) \
    $(wildcard include/config/bridge/netfilter.h) \
    $(wildcard include/config/vlan/8021q.h) \
    $(wildcard include/config/vlan/8021q/module.h) \
    $(wildcard include/config/netfilter/debug.h) \
    $(wildcard include/config/hippi.h) \
    $(wildcard include/config/net/sched.h) \
    $(wildcard include/config/net/cls/act.h) \
  include/linux/highmem.h \
  include/asm/cacheflush.h \
  include/asm/cpu-features.h \
  include/asm-mips/mach-generic/cpu-feature-overrides.h \
  include/linux/poll.h \
  include/asm/poll.h \
  include/asm/uaccess.h \
  include/linux/net.h \
  include/net/checksum.h \
  include/asm/checksum.h \
  include/linux/in6.h \
  include/linux/netlink.h \
  include/linux/socket.h \
  include/asm/socket.h \
  include/asm/sockios.h \
  include/linux/sockios.h \
  include/linux/uio.h \
  include/asm/unistd.h \
  ipc/util.h \

ipc/util.o: $(deps_ipc/util.o)

$(deps_ipc/util.o):
