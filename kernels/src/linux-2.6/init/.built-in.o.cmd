cmd_init/built-in.o :=  mipsel-linux-ld  --oformat elf32-tradlittlemips  -r -o init/built-in.o init/main.o init/version.o init/mounts.o init/initramfs.o
