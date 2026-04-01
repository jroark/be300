/*
 * BRIEF MODULE DESCRIPTION
 *	Board specific pci fixups.
 *
 * Copyright 2001 MontaVista Software Inc.
 * Author: MontaVista Software, Inc.
 *         	ppopov@mvista.com or source@mvista.com
 *
 * Ported to Markham (Korva) by Geoffrey Espin espin@idiom.com for NEC.
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 *  THIS  SOFTWARE  IS PROVIDED   ``AS  IS'' AND   ANY  EXPRESS OR IMPLIED
 *  WARRANTIES,   INCLUDING, BUT NOT  LIMITED  TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
 *  NO  EVENT  SHALL   THE AUTHOR  BE    LIABLE FOR ANY   DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 *  NOT LIMITED   TO, PROCUREMENT OF  SUBSTITUTE GOODS  OR SERVICES; LOSS OF
 *  USE, DATA,  OR PROFITS; OR  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 *  ANY THEORY OF LIABILITY, WHETHER IN  CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 *  THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the  GNU General Public License along
 *  with this program; if not, write  to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/init.h>

#include "pinot.h"

#define	DEBUG
#undef	DEBUG
#ifdef 	DEBUG
#define	DBG(x...)	printk(x)
#else
#define	DBG(x...)
#endif

void __init
pcibios_fixup_resources(struct pci_dev *dev)
{
	u8 gnt;
	u8 lat;

	pci_read_config_byte(dev, PCI_MIN_GNT, &gnt);
	pci_read_config_byte(dev, PCI_MAX_LAT, &lat);

	/* dev->dma_mask = 0x003fffff;  * 4M */

	pci_write_config_byte(dev, PCI_MIN_GNT, 0xff);
	pci_write_config_byte(dev, PCI_MAX_LAT, 0xff);

	/* the PINOT requires addresses starting at 0xb0100000 */
	pci_write_config_dword(dev, PCI_BASE_ADDRESS_0, PINOT_IOMEM);
	pci_write_config_dword(dev, PCI_BASE_ADDRESS_1, PINOT_IOMEM);
}

void __init
pcibios_fixup(void)
{
	/* ditto setup.c */

	*P_PCI_COMMAND = (0xffff0000 | PCI_COMMAND_MEMORY |
			  PCI_COMMAND_MASTER | PCI_COMMAND_INVALIDATE |
			  PCI_COMMAND_PARITY | PCI_COMMAND_SERR);

	*P_PCI_CLASS_REVISION = 0x06000000;	/* HOST MODE */
	*P_PCI_CACHE_LINE_SIZE |= 0x04ff0000;	/* 4 words */
	*P_PCI_BASE_ADDRESS_REG = PINOT_REG_BASE_PHYS;	/* (0x14) */

	*P_PCI_BASE_ADDRESS_MEM = PINOT_MEM_START;	/* (0x10) 4M at top of mem */
	*P_PCI_PM_DATA_CSR = 0;	/* ? */

	*P_IBBA = PINOT_MEM_START;	/* 4M at top of mem */
	*P_PLBA = PINOT_IOMEM;	/* 0x00100000 */

	*P_BCNT = 0x80000020;	/* INITDONE + ICMDS */
}

void __init
pcibios_fixup_irqs(void)
{
	struct pci_dev *dev;
	volatile u32 tmp;

	pci_for_each_dev(dev) {
		if (dev->bus->number != 0)
			return;

		dev->irq = 5;	/* IP5: PCI shared with CANDY #2 */
		pci_write_config_byte(dev, PCI_INTERRUPT_LINE, dev->irq);
	}

	tmp = *P_IGSR;		/* clear IBUS GSR */
	*P_IIMR = 0x800;	/* unmask interrupts */
}

unsigned int __init
pcibios_assign_all_busses(void)
{
	return 1;
}

/* ../kernel/pci-dma.c replacement */

#warning PCI ALLOC: see linux/net/core/skbuff.c

#include <linux/list.h>

#define	LOCK_INTS 1

#define list_init(x) INIT_LIST_HEAD(x)

struct list_head p_active_list;
struct list_head p_free_list;

typedef struct {
	struct list_head list;	/* 8 */
	int size;		/* 4 */
	u8 *addr;		/* 4 */
	u8 buf[0];		/* 16 byte boundary */
} PINOT_HDR;

#define	P_HDR(buf)	(PINOT_HDR *)((size_t)(buf) - sizeof(PINOT_HDR))
#define	P_BUF(hdr)	((u8 *)((size_t)(hdr) + sizeof(PINOT_HDR)))

u8 *p_mem = (u8 *)
    KSEG1ADDR(PINOT_MEM_START) +
    PAGE_ALIGN(1) -
    sizeof (PINOT_HDR);		/* skb's page aligned */
u8 *p_mem_end = (u8 *)
    KSEG1ADDR(PINOT_MEM_END);

void *
korva_pci_alloc_consistent(struct pci_dev *dev, size_t size, dma_addr_t * da)
{
	static int done = 0;
	struct list_head *p;
	PINOT_HDR *ph = NULL;

#if LOCK_INTS
	ulong flags;
	save_flags(flags);
	cli();
#endif

	if (!done) {
		done = 1;
		list_init(&p_free_list);
		list_init(&p_active_list);
	}

	size += sizeof (PINOT_HDR);

	list_for_each(p, &p_free_list) {
		ph = list_entry(p, PINOT_HDR, list);
		if (size <= ph->size) {
			list_del(p);
			break;	/* found one on free list, remove it... */
		}
		ph = NULL;
	}

	if (ph == NULL) {
		/* none found, so get a fresh one from high mem */
		size = PAGE_ALIGN(size);
		ph = (PINOT_HDR *) p_mem;
		ph->size = size;
		p_mem += size;
		if ((u32) p_mem >= (u32) p_mem_end) {
#if LOCK_INTS
			restore_flags(flags);
#endif
			printk(__FILE__ ": out of high PCI memory!\n");
			return (NULL);
		}
		if (da)
			*da = (dma_addr_t) virt_to_bus(&ph->buf);
		p = &ph->list;
	}

	/* now its in play */
	list_add(p, &p_active_list);

#if LOCK_INTS
	restore_flags(flags);
#endif
	return ((void *) ph->buf);
}

void
korva_pci_free_consistent(struct pci_dev *dev, size_t size, void *va,
			  dma_addr_t da)
{
	struct list_head *p;
	PINOT_HDR *ph;

	if (KSEGX(va) == KSEG1) {
#if LOCK_INTS
		ulong flags;
		save_flags(flags);
		cli();
#endif
		ph = P_HDR(va);
		p = &ph->list;
		list_del(p);
		list_add(p, &p_free_list);
#if LOCK_INTS
		restore_flags(flags);
#endif
	} else {
		printk("%s:%s#%3d %s %#x [%#x %#x %#x %#x]\n",
		       __FILE__, __FUNCTION__, __LINE__,
		       "not KSEG1", (int) va,
		       ((int *) va)[0], ((int *) va)[1], ((int *) va)[2],
		       ((int *) va)[3]);
		if (0)
			list_for_each(p, &p_active_list) {
			ph = list_entry(p, PINOT_HDR, list);
			if ((u8 *) KSEG1ADDR(va) == ph->buf)
				printk("*** ");
			printk("%#x %#x, ", (int) p, (int) ph->buf);
			}
	}
}
