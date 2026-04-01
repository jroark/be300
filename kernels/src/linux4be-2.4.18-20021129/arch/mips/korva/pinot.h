/* pinot.h */

#include <linux/pci.h>

/*
From: Daisaku_Itasaka@el.nec.com

Example PCI mapping:

    b010_0000 +-------------+
              |             | Device A's I/O (size 0x100)
    b010_0100 +-------------+
              |             | Device A's MEM (size 0x2000)
    b010_2100 +-------------+
              |             | Device B's I/O (size 0x40)
    b010_2140 +-------------+
              |             | Device B's MEM (size 0x10000)
    b011_2140 +-------------+

PINOT writes each device's PCI Configuration Space with:

    Device A  +-------------+
              |             |
              +-------------+ 0x10
              | 00 10 00 00 |         I/O addr
              +-------------+ 0x14
              | 00 10 01 00 |         MEM addr
              +-------------+ 0x18

    Device B  +-------------+
              |             |
              +-------------+ 0x10
              | 00 10 21 00 |         I/O addr
              +-------------+ 0x14
              | 00 10 21 40 |         MEM addr
              +-------------+ 0x18

This way, when each device's own device driver reads its own
PCI Configuration Space, they get proper addresses. For example,
Device B gets 00102100 for I/O address base. When the driver
tries to access his own device register, he would call:

	  inb (0x00102100);

This will be converted to 0xb0102100, so that PINOT can take it
and forward it to the PCI bus. Any access to PCI must go
through 0xb0100000 Window. That's why PINOT must initialize
each device's configuration space with something beyond 0x100000.

The MEM resource is something that you (as a device) want to
expose to the driver as a memory-mapped resource. If an I/O
access uses a different mechanism than the memory access,
it would make sense. On Intel chip, they are different. For
I/O access, he uses designated CPU instruction. But, MIPS
has no distinction. There's no I/O. Everything is memory-mapped.
So, in that case, I/O and MEM resource have no difference.

And this MEM resource has nothing to do with our upper 4MB
SDRAM space. This is rather device's own resource. It's on
the device. It is how you want to access the device. Not
what the device can access. That is what a device wants
to be accessed via memory-mapped scheme, not I/O.
And they don't differ on MIPS. So we map everything on
memory (0xb0100000).
*/


#define PINOT_REG_BASE_PHYS	(0x10004000)
#define PINOT_REG_BASE		(KSEG1|PINOT_REG_BASE_PHYS)
#define PINOT_CONFIG_BASE	(KSEG1|PINOT_REG_BASE_PHYS|0x100)
#define PINOT_IOMEM   		(0x00100000)			/* 0xb0100000 */

#define PCI_IO_SIZE		0x00001000                     	/*  4K size */
#define PCI_IO_START		PINOT_IOMEM
#define PCI_IO_END		(PCI_IO_START+PCI_IO_SIZE-1)

#define PCI_IOMEM_SIZE  	0x00200000			/* 2M */
#define PCI_IOMEM_START		(PCI_IO_END+1)	/* */
#define PCI_IOMEM_END		(PCI_IOMEM_START+PCI_IOMEM_SIZE-1)

/* 4M; see prom.c,pci_ops.c */
#define PINOT_MEM_SIZE  	0x00400000			/* 4M */
#define PINOT_MEM_START		(0x02000000-PINOT_MEM_SIZE)	/* 32M - 4M */
#define PINOT_MEM_END		(PINOT_MEM_START+PINOT_MEM_SIZE-1)

#define	P_(x) (volatile u32 *)(x)	/* PINOT pointer */

#define	P_PLBA	P_(PINOT_REG_BASE|0x00)	/* PCI Lower Base Address Register */
#define	P_PUBA	P_(PINOT_REG_BASE|0x04)	/* PCI Upper Base Address Register */
#define	P_IBBA	P_(PINOT_REG_BASE|0x08)	/* IBUS Base Addess Register */
#define	P_VERR	P_(PINOT_REG_BASE|0x10)	/* Version Register */
#define	P_PCAR	P_(PINOT_REG_BASE|0x14)	/* PCI Configuration Address Register */
#define	P_PCDR	P_(PINOT_REG_BASE|0x18)	/* PCI Configuration Data Register */
#define	P_IGSR	P_(PINOT_REG_BASE|0x1c)	/* IBUS General Status Register */
#define	P_IIMR	P_(PINOT_REG_BASE|0x20)	/* IBUS Interrupt Mask Register */
#define	P_PGSR	P_(PINOT_REG_BASE|0x24)	/* PCI General Status Register */
#define	P_PIMR	P_(PINOT_REG_BASE|0x28)	/* PCI Interrupt Mask Register */
#define	P_HMCR	P_(PINOT_REG_BASE|0x30)	/* Host Mode Control Register */
#define	P_PWCD	P_(PINOT_REG_BASE|0x40)	/* Power Consumption Data Register */
#define	P_PWDD	P_(PINOT_REG_BASE|0x44)	/* Power Dissipation Data Register */
#define	P_BCNT	P_(PINOT_REG_BASE|0x50)	/* Bridge Control Register */
#define	P_PPCR	P_(PINOT_REG_BASE|0x54)	/* Power Control Register */
#define	P_SWRR	P_(PINOT_REG_BASE|0x58)	/* Software Reset Register */
#define	P_TRMR	P_(PINOT_REG_BASE|0x5c)	/* Retry Timer Register */

#define	P_PCI_COMMAND		P_(PINOT_CONFIG_BASE|PCI_COMMAND)
#define	P_PCI_CLASS_REVISION	P_(PINOT_CONFIG_BASE|PCI_CLASS_REVISION)
#define	P_PCI_CACHE_LINE_SIZE	P_(PINOT_CONFIG_BASE|PCI_CACHE_LINE_SIZE)
#define	P_PCI_BASE_ADDRESS_MEM	P_(PINOT_CONFIG_BASE|PCI_BASE_ADDRESS_0)
#define	P_PCI_BASE_ADDRESS_REG	P_(PINOT_CONFIG_BASE|PCI_BASE_ADDRESS_1)

#define	P_PCI_PM_DATA_CSR	P_(PINOT_CONFIG_BASE|0x44)

/* Internal Register - PCI Configuration Address Register (18H R/W)         */
#define CFGTYPE0    0x00000000      /* Configuration Type0                  */
#define CFGTYPE1    0x00000001      /* Configuraiton Type1                  */
#define DEV0        0x00000000      /* Device number 0                      */
#define DEV1        0x00000800      /* Device number 1                      */
#define DEV2        0x00001000      /* Device number 2                      */
#define CFGEN       0x80000000      /* Configuration Cycle Enable           */

/* Internal Register - IBUS General Status Register (20H R)                 */
#define INTA        0x80000000      /* PCI interrupt INTA# (host mode only) */
#define PCIRST      0x40000000      /* PCI Reset RST# (nic mode only)       */
#define DPPE        0x20000000      /* Detected PCI Parity Error            */
#define PSER        0x10000000      /* PCI System Error received ( host mode only) */
#define PCIUP       0x02000000      /* PCI host requests to resume PINOT    */
#define PCIDOWN     0x01000000      /* PCI host requests to suspend PINOT   */
#define DMAEND      0x00010000      /* DMA operation has finished           */
#define BUSERR      0x00008000      /* IBUS error when PINOT is IBUS master */
#define IBUSFD      0x00000080      /* IBUS FIFO discarded                  */
#define IDLTA       0x00000008      /* IDL Target Abort                     */
#define PWTA        0x00000004      /* PW Target Abort                      */
#define IDLMA       0x00000002      /* IDL Master Abort                     */
#define PWMA        0x00000001      /* PW Master Abort                      */

/* Internal Register - IBUS Interrupt Mask Register (24H R/W)               */
#define IGSRMASK    0xf301808f

/* Internal Register - Host Mode Control (50H R/W)                          */
#define ROTAMODE    0x00000001      /* PCI Arbiter Rotating mode            */
#define RSTOUT      0x80000000      /* PCI BUS Reset out                    */
#define CLKOUT0     0x00000010      /* Clock output 0 enable                */
#define CLKOUT1     0x00000020      /* Clock output 0 enable                */
#define CLKOUT2     0x00000040      /* Clock output 0 enable                */
#define CLKOUT3     0x00000080      /* Clock output 0 enable                */
#define CLKOUT      0x000000f0      /* Clock output all enable              */

/* Internal Register - DMA Command Register (60H R/W)                       */
#define DMAEN       0x00000001
#define IDBS16W     0x00000008
#define GA_DMA      0x00000010
#define DMAST       0x80000000

/* end of pinot.h */

