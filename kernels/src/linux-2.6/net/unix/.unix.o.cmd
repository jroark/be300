cmd_net/unix/unix.o := mipsel-linux-ld  --oformat elf32-tradlittlemips  -r -o net/unix/unix.o net/unix/af_unix.o net/unix/garbage.o net/unix/sysctl_net_unix.o
