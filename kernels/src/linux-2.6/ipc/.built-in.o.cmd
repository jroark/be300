cmd_ipc/built-in.o :=  mipsel-linux-ld  --oformat elf32-tradlittlemips  -r -o ipc/built-in.o ipc/util.o ipc/msgutil.o ipc/msg.o ipc/sem.o ipc/shm.o
