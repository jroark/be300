/*
 *
 * BRIEF MODULE DESCRIPTION
 * 	RC32334 DMA controller, with scatter/gather support
 *
 * Copyright 2001,2002 THOMSON multimedia.
 * Author: Stephane Fillod
 *         	fillods@thmulti.com
 *
 * Parts from mips/kernel/pci-dma.c:
 * Copyright (C) 2000  Ani Joshi <ajoshi@unixbox.com>
 * Copyright (C) 2000  Ralf Baechle <ralf@gnu.org>
 * swiped from i386, and cloned for MIPS by Geert, polished by Ralf.
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


#include <linux/config.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <asm/rc32300/rc32300.h>
#include <asm/rc32300/rc32334_dma.h>
#include <asm/io.h>


/* #define IDTDMA_DEBUG 1 */

/*
 * a single transfer transaction would require at least TWO descriptors, 
 * i.e. one for the transfert itself, and one for the dummy descriptor.
 * But we're smart, and the dummy descriptor is shared among DMA channels.
 */
static rc32334_dma_desc_t dma_dummy_descriptor = {
	0, DMA_DUMMY_ADDR, DMA_DUMMY_ADDR, DMA_DUMMY_ADDR
};


static struct {
	rc32334_dma_desc_t single_desc;
	void (*cb)(void *); // DMA completion callback
	void *cb_arg;       // arg to callback
	int irq;            // IRQ assigned to this channel
} dma_ch[RC32334_MAX_DMA_CHANNELS];


/* 
 * actualy, hwdev is ignored
 */
void *rc32334_alloc_consistent(void *hwdev, size_t size,
			       dma_addr_t * dma_handle)
{
	void *ret;
	int gfp = GFP_ATOMIC;

	ret = (void *) __get_free_pages(gfp, get_order(size));

	if (ret != NULL) {
		memset(ret, 0, size);
		dma_cache_wback_inv((unsigned long) ret, size);
		*dma_handle = virt_to_bus(ret);
		ret = (void*)KSEG1ADDR(ret);
	}

	return ret;
}

void rc32334_free_consistent(void *hwdev, size_t size,
			     void *vaddr, dma_addr_t dma_handle)
{
	free_pages((unsigned long) KSEG0ADDR(vaddr), get_order(size));
}


/*
 * Sharing the same handler among DMA channals is good since it maximises
 * the i-cache use.
 *
 * Keep in mind, this is a fast interrupt handler (all interrupts masked)
 *
 */
static void rc32334_dma_interrupt(int irq, void *dev_id,
				  struct pt_regs * regs)
{
	int dmanr;
	void (*dma_cb)(void*);

	//dmanr = irq - RC32334_IRQ_DMA0;
	dmanr = (int)dev_id;

	if (dmanr < 0 || dmanr >= RC32334_MAX_DMA_CHANNELS) {
		printk(KERN_ERR "Invalid DMA interrupt number %d\n", dmanr);
		return;		/* FIXME: bail out ? account for spurious? */
	}

	/*
	 * TODO: check PENDREG here to check for abnormal conditions
	 */

	if (dma_ch[dmanr].cb) {
		dma_cb = dma_ch[dmanr].cb;
		dma_ch[dmanr].cb = NULL;
		(*dma_cb)(dma_ch[dmanr].cb_arg);
	}
}


/*
 * Setup DMA single transfer, to be in appropriate state for
 * rc32334_dma_initiate_single().
 * Status argument is taken from ORing various DMA_* defines depending on
 * your needs.
 * The default is src is incrementing, destination is incrementing, source and 
 * destination are little-endian, no completion interruption requested.
 *
 * Whenever DMA_DONE_INT bit is enabled as part of status arg, the dma_done_cb
 * pointer to function must not be NULL, and will be called upon completion of
 * the DMA transfers. Keep in mind that dma_done_cb will be called from an
 * interrupt context with all interrupts masked (not allowed to sleep, etc.) 
 * and thus, it must behave (Groovy baby).
 *
 * NB: rc32334_dma_setup_single is limited to 64KB-1 max transfer!
 */
void rc32334_dma_setup_single(unsigned int dmanr, dma_addr_t src,
			      dma_addr_t dst, size_t size, int status,
			      void (*dma_done_cb)(void *), void *arg)
{
	int retval;
	u32 dma_status;
	rc32334_dma_desc_t *dma_sdesc;

#ifdef IDTDMA_DEBUG

	if (!src || !dst)
		BUG();

	/* check for invalid size. This DMA can not do full 64KB transfers */
	if (size > DMA_MAX_TRANSZ)
		BUG();

	/* check for non exported bits */
	if (status & 0xf000ffff)
		BUG();

	/* check for invalid modes */
	if ( ((status & DMA_SRC_DEC) && !(status & DMA_DST_DEC)) ||
	     ((status & DMA_DST_DEC) && !(status & DMA_SRC_DEC)) )
		BUG();
	/* can't be constant and decrement */
	if ((status & (DMA_SRC_CONST|DMA_SRC_DEC)) ==
	    (DMA_SRC_CONST|DMA_SRC_DEC))
		BUG();
	if ((status & (DMA_DST_CONST|DMA_DST_DEC)) ==
	    (DMA_DST_CONST|DMA_DST_DEC))
		BUG();

	if ((status & DMA_DONE_INT) && !dma_done_cb)
		BUG();
#endif	/* IDTDMA_DEBUG */

	/* disable DMA */
	rc32300_writel(0UL, DMA_CONFREG(dmanr));

	if (dma_ch[dmanr].irq == 0) {
		/* "fast" interrupt */
		retval = request_irq(DMA_IRQ(dmanr), rc32334_dma_interrupt,
				     SA_INTERRUPT, "RC32334 DMA",
				     (void*)dmanr);
		if (retval) {
			printk(KERN_ERR __FUNCTION__
			       ": request_irq error %d\n", retval);
			return;
		}

		dma_ch[dmanr].irq = DMA_IRQ(dmanr);
	}
	
	dma_sdesc = &dma_ch[dmanr].single_desc;

	/* init desc 0: last */
	dma_status = DMA_DMAOWNER | DMA_LASTDESC | status | size;

	dma_sdesc->status = dma_status;

	dma_sdesc->src_addr = src;
	dma_sdesc->dst_addr = dst;

	/*
	 * can't be done at init since the DMA controller will update
	 * 	this field
	 */
	dma_sdesc->next = virt_to_bus(&dma_dummy_descriptor);

	dma_cache_wback_inv((unsigned long) dma_sdesc,
			    sizeof(rc32334_dma_desc_t));

#if 0
	printk("%08x %p %08x %08x %08x\n",
	       virt_to_bus(&dma_ch[dmanr].single_desc),
	       dma_sdesc, dma_sdesc->src_addr, dma_sdesc->dst_addr,
	       dma_sdesc->next);
#endif

	/* setup base desc register */
	rc32300_writel(virt_to_bus(dma_sdesc), DMA_BASEREG(dmanr));

	/* setup interrupt controller */
	dma_ch[dmanr].cb = dma_done_cb;
	dma_ch[dmanr].cb_arg = arg;
}
 
/*
 * cfg param can control Restart, DMArdy and burst size
 * see asm/rc32300/rc32334_dma.h for more information
 * NB: on 79S334, DMAready is only available for dmanr 0 and 1.
 */
void rc32334_dma_initiate_single(unsigned int dmanr, unsigned int cfg)
{
	/* Enable DMA, and go! */

	rc32300_writel(DMA_ENABLE | (cfg & 0x5f000000), DMA_CONFREG(dmanr));
}

/*
 * ditto for scatter/gather
 *
 * Note: sgdescs cannot point to a translated address. 
 * a buffer allocated with kmalloc(nents*sizeof(rc32334_dma_desc_t)) is fine
 */
void rc32334_dma_setup_sg(unsigned int dmanr, rc32334_dma_desc_t *sgdescs,
			  const struct scatterlist *sg, dma_addr_t addr,
			  int nents, int status, void (*dma_done_cb)(void *),
			  void *arg)
{
	u32 dma_status;
	rc32334_dma_desc_t *dma_desc;
	int i, retval;

#ifdef IDTDMA_DEBUG

	if (!sgdescs || !sg || !addr)
		BUG();

	/* check for non exported bits */
	if (status & 0xf000ffff)
		BUG();

	/* Kludge: if it's constant and decrement, this means addr is
	 * src/dst and incrementing (DMA_FROM_ADDR/DMA_TO_ADDR).
	 * pretty hairy, huh!
	 */

	/* This Linux implementation does not support sg to sg memcopy,
	 * this is to make the function prototype much more effective,
	 * since this feature is only used seldomly.
	 */
	if ((status & (DMA_SRC_CONST|DMA_DST_CONST)) == 0)
		BUG();
		                    
	if ((status & DMA_DONE_INT) && !dma_done_cb)
		BUG();
#endif

	/* disable DMA */
	rc32300_writel(0UL, DMA_CONFREG(dmanr));

	if (dma_ch[dmanr].irq == 0) {
		/* "fast" interrupt */
		retval = request_irq(DMA_IRQ(dmanr), rc32334_dma_interrupt,
				     SA_INTERRUPT, "RC32334 DMA",
				     (void*)dmanr);
		if (retval) {
			printk(KERN_ERR __FUNCTION__
			       ": request_irq error %d\n", retval);
			return;
		}

		dma_ch[dmanr].irq = DMA_IRQ(dmanr);
	}

	dma_desc = sgdescs;

	for (i=0; i < nents-1; i++) {

#ifdef IDTDMA_DEBUG

		/*
		 * check for invalid size. The 79S334 DMA can not do
		 * full 64KB transfers
		 */
		if (sg_dma_len(sg) > DMA_MAX_TRANSZ) {
			printk(KERN_CRIT "Invalid sg%d/%d, len: %d\n",
			       i, nents, sg_dma_len(sg));
			BUG();
		}
#endif

		dma_status =
			DMA_DMAOWNER | (status&~DMA_DONE_INT) |	sg_dma_len(sg);
		if ((status & DMA_FROM_ADDR) == DMA_FROM_ADDR)
			dma_status &= ~DMA_FROM_ADDR;
		if ((status & DMA_TO_ADDR) == DMA_TO_ADDR)
			dma_status &= ~DMA_TO_ADDR;

		dma_desc->status = dma_status;

		if (status & DMA_SRC_CONST) {
			dma_desc->src_addr = addr;
			dma_desc->dst_addr = sg_dma_address(sg);
		} else if (status & DMA_DST_CONST) {
			dma_desc->src_addr = sg_dma_address(sg);
			dma_desc->dst_addr = addr;
		} else  {
			BUG();
		}
		
		if ((status & DMA_FROM_ADDR) == DMA_FROM_ADDR ||
		    (status & DMA_TO_ADDR) == DMA_TO_ADDR) {
			addr += sg_dma_len(sg);
		} 

		dma_desc->next = virt_to_bus(dma_desc+1);
		
		dma_desc++;
		sg++;
	}


	/* handle the last descriptor outside the loop */

	dma_status = DMA_DMAOWNER | DMA_LASTDESC | status | sg_dma_len(sg);
	if ((status & DMA_FROM_ADDR) == DMA_FROM_ADDR)
		dma_status &= ~DMA_FROM_ADDR;
	if ((status & DMA_TO_ADDR) == DMA_TO_ADDR)
		dma_status &= ~DMA_TO_ADDR;
	dma_desc->status = dma_status;

	if (status & DMA_SRC_CONST) {
		dma_desc->src_addr = addr;
		dma_desc->dst_addr = sg_dma_address(sg);
	} else {
		dma_desc->src_addr = sg_dma_address(sg);
		dma_desc->dst_addr = addr;
	}

	/* end the chain */
	dma_desc->next = virt_to_bus(&dma_dummy_descriptor);

	dma_cache_wback_inv((unsigned long) sgdescs,
			    nents*sizeof(rc32334_dma_desc_t));

	/* setup base desc register */
	rc32300_writel(virt_to_bus(sgdescs), DMA_BASEREG(dmanr));

	/* install interrupt completion call back */
	dma_ch[dmanr].cb = dma_done_cb;
	dma_ch[dmanr].cb_arg = arg;
}

/*
 * same as rc32334_dma_initiate_single()
 */
void rc32334_dma_initiate_sg(unsigned int dmanr, unsigned int cfg)
{
	/* Enable DMA and go! */

	rc32300_writel(DMA_ENABLE | (cfg & 0x5f000000), DMA_CONFREG(dmanr));
}

void rc32334_dma_disable(unsigned int dmanr)
{
	rc32300_writel(0UL, DMA_CONFREG(dmanr));
}

EXPORT_SYMBOL(rc32334_alloc_consistent);
EXPORT_SYMBOL(rc32334_free_consistent);
EXPORT_SYMBOL(rc32334_dma_setup_single);
EXPORT_SYMBOL(rc32334_dma_initiate_single);
EXPORT_SYMBOL(rc32334_dma_setup_sg);
EXPORT_SYMBOL(rc32334_dma_initiate_sg);
EXPORT_SYMBOL(rc32334_dma_disable);
