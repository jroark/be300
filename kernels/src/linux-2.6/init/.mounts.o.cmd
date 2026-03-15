cmd_init/mounts.o := mipsel-linux-ld  --oformat elf32-tradlittlemips  -r -o init/mounts.o init/do_mounts.o init/do_mounts_rd.o init/do_mounts_initrd.o
