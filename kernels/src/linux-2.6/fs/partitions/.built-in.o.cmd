cmd_fs/partitions/built-in.o :=  mipsel-linux-ld  --oformat elf32-tradlittlemips  -r -o fs/partitions/built-in.o fs/partitions/check.o fs/partitions/msdos.o
