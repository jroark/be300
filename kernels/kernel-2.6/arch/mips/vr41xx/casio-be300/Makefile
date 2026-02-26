#
# Makefile for the Casio Cassiopeia BE-300 specific parts of the kernel
#
# Note! Dependencies are done automagically by 'make dep', which also
# removes any old dependencies. DON'T put your own dependencies here
# unless it's something special (ie not a .c file).
#

.S.s:
	$(CPP) $(AFLAGS) $< -o $@
.S.o:
	$(CC) $(AFLAGS) -c $< -o $@

obj-y	 := prom.o setup.o debug.o
# ide-be300.o

obj-$(CONFIG_PCI) += pci_fixup.o
