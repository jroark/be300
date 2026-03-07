cmd_net/ethernet/built-in.o :=  mipsel-linux-ld  --oformat elf32-tradlittlemips  -r -o net/ethernet/built-in.o net/ethernet/eth.o net/ethernet/sysctl_net_ether.o
