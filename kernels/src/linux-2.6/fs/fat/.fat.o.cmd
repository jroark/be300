cmd_fs/fat/fat.o := mipsel-linux-ld  --oformat elf32-tradlittlemips  -r -o fs/fat/fat.o fs/fat/cache.o fs/fat/dir.o fs/fat/file.o fs/fat/inode.o fs/fat/misc.o fs/fat/fatfs_syms.o
