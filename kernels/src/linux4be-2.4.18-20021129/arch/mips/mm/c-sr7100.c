/*
  Jason Gunthorpe <jgg@yottayotta.com>
  Copyright (C) 2002 YottaYotta. Inc.
   
  Based on c-mips32:
  Kevin D. Kissell, kevink@mips.com and Carsten Langgaard, carstenl@mips.com
  Copyright (C) 2000 MIPS Technologies, Inc.  All rights reserved.

  This file is subject to the terms and conditions of the GNU General Public
  License.  See the file "COPYING" in the main directory of this archive
  for more details.
   
  Sandcraft SR7100 cache routines. 
  The SR7100 is a MIPS64 compatible CPU with 4 caches:
   * 4 way 32K primary ICache - virtually indexed/physically tagged
   * 4 way 32K primary DCache - virtually indexed/physically tagged
   * 8 way 512K secondary cache - physically indexed/taged
   * 8 way up to 16M tertiary cache - physically indexed/taged (and off chip)
   
  ICache and DCache do not have any sort of snooping. Unlike the RM7k,
  the virtual index is 13 bits, and we use a 4k page size. This means you 
  can have cache aliasing effects, so they have to be treated as virtually
  tagged. (unless that can be solved elsewhere, should investigate)

  Note that on this chip all the _SD type cache ops (ie Hit_Writeback_Inv_SD)
  are really just _S. This is in line with what the MIPS64 spec permits.
  Also, the line size of the tertiary cache is really the block size. The
  line size is always 32 bytes. The chip can tag partial blocks and the cache
  op instructions work on those partial blocks too. 
  
  See ./Documentation/cachetlb.txt
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>

#include <asm/bootinfo.h>
#include <asm/cpu.h>
#include <asm/bcache.h>
#include <asm/io.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/system.h>
#include <asm/mmu_context.h>

#undef DEBUG_CACHE

/* Primary cache parameters. */
int icache_size, dcache_size; 			/* Size in bytes */
int ic_lsize, dc_lsize;				/* LineSize in bytes */

/* Secondary cache parameters. */
unsigned int scache_size, sc_lsize;		/* Again, in bytes */

/* tertiary cache (if present) parameters. */
unsigned int tcache_size, tc_lsize;		/* Again, in bytes */

#include <asm/cacheops.h>
#include <asm/mips32_cache.h>

// Unique to the SR7100
#define Index_Invalidate_T 0x2
#define Hit_Invalidate_T 0x16

static inline void blast_tcache(void)
{
	unsigned long start = KSEG0;
	unsigned long end = KSEG0 + tcache_size;
	
	// Could use flash invalidate perhaps..
	while(start < end)
	{
		cache_unroll(start,Index_Invalidate_T);
		start += tc_lsize;
	}	
}

static inline void flush_tcache_line(unsigned long addr)
{
	__asm__ __volatile__
		(
		 ".set noreorder\n\t"
		 ".set mips3\n\t"
		 "cache %1, (%0)\n\t"
		 ".set mips0\n\t"
		 ".set reorder"
		 :
		 : "r" (addr),
		 "i" (Hit_Invalidate_T));
}

/*
 * Dummy cache handling routines for machines without boardcaches
 */
static void no_sc_noop(void) {}

static struct bcache_ops no_sc_ops = {
	(void *)no_sc_noop, (void *)no_sc_noop,
	(void *)no_sc_noop, (void *)no_sc_noop
};
struct bcache_ops *bcops = &no_sc_ops;

// Clean all virtually indexed caches
static inline void sr7100_flush_cache_all_pc(void)
{
	unsigned long flags;

	__save_and_cli(flags);
	blast_dcache(); blast_icache();
	__restore_flags(flags);
}

// This clears all caches. It is only used from a syscall..
static inline void sr7100_nuke_caches(void)
{
	unsigned long flags;

	__save_and_cli(flags);
	blast_dcache(); blast_icache(); blast_scache();
	if (tcache_size != 0)
		blast_tcache();
	__restore_flags(flags);
}

/* This is called to clean out a virtual mapping. We only need to flush the
   I and D caches since the other two are physically tagged */
static void sr7100_flush_cache_range_pc(struct mm_struct *mm,
				     unsigned long start,
				     unsigned long end)
{
	if(mm->context != 0) {
		unsigned long flags;

#ifdef DEBUG_CACHE
		printk("crange[%d,%08lx,%08lx]", (int)mm->context, start, end);
#endif
		__save_and_cli(flags);
		blast_dcache(); blast_icache();
		__restore_flags(flags);
	}
}

/*
 * On architectures like the Sparc, we could get rid of lines in
 * the cache created only by a certain context, but on the MIPS
 * (and actually certain Sparc's) we cannot.
 * Again, only clean the virtually tagged cache.
 */
static void sr7100_flush_cache_mm_pc(struct mm_struct *mm)
{
	if(mm->context != 0) {
#ifdef DEBUG_CACHE
		printk("cmm[%d]", (int)mm->context);
#endif
		sr7100_flush_cache_all_pc();
	}
}

static void sr7100_flush_cache_page_pc(struct vm_area_struct *vma,
				    unsigned long page)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long flags;
	pgd_t *pgdp;
	pmd_t *pmdp;
	pte_t *ptep;

	/*
	 * If ownes no valid ASID yet, cannot possibly have gotten
	 * this page into the cache.
	 */
	if (mm->context == 0)
		return;

#ifdef DEBUG_CACHE
	printk("cpage[%d,%08lx]", (int)mm->context, page);
#endif
	__save_and_cli(flags);
	page &= PAGE_MASK;
	pgdp = pgd_offset(mm, page);
	pmdp = pmd_offset(pgdp, page);
	ptep = pte_offset(pmdp, page);

	/*
	 * If the page isn't marked valid, the page cannot possibly be
	 * in the cache.
	 */
	if (!(pte_val(*ptep) & _PAGE_VALID))
		goto out;

	/*
	 * Doing flushes for another ASID than the current one is
	 * too difficult since Mips32 caches do a TLB translation
	 * for every cache flush operation.  So we do indexed flushes
	 * in that case, which doesn't overly flush the cache too much.
	 */
	if (mm == current->active_mm) {
		blast_dcache_page(page);
	} else {
		/* Do indexed flush, too much work to get the (possible)
		 * tlb refills to work correctly.
		 */
		page = (KSEG0 + (page & (dcache_size - 1)));
		blast_dcache_page_indexed(page);
	}
out:
	__restore_flags(flags);
}

/* If the addresses passed to these routines are valid, they are
 * either:
 *
 * 1) In KSEG0, so we can do a direct flush of the page.
 * 2) In KSEG2, and since every process can translate those
 *    addresses all the time in kernel mode we can do a direct
 *    flush.
 * 3) In KSEG1, no flush necessary.
 */
static void sr7100_flush_page_to_ram_pc(struct page *page)
{
	blast_dcache_page((unsigned long)page_address(page));
}

/* I-Cache and D-Cache are seperate and virtually tagged, these need to
   flush them */
static void sr7100_flush_icache_range(unsigned long start, unsigned long end)
{
	flush_cache_all();  // only does i and d, probably excessive
}

static void sr7100_flush_icache_page(struct vm_area_struct *vma,
				     struct page *page)
{
	int address;

	if (!(vma->vm_flags & VM_EXEC))
		return;

	address = KSEG0 + ((unsigned long)page_address(page) & PAGE_MASK & (dcache_size - 1));
	blast_icache_page_indexed(address);
}

/* Writeback and invalidate the primary cache dcache before DMA.
   See asm-mips/io.h 
 */
static void sr7100_dma_cache_wback_inv_sc(unsigned long addr,
					  unsigned long size)
{
	unsigned long end, a;

	if (size >= scache_size) {
		sr7100_nuke_caches();
		return;
	}

	a = addr & ~(sc_lsize - 1);
	end = (addr + size) & ~(sc_lsize - 1);
	while (1) {
		flush_dcache_line(a);
		flush_scache_line(a); // Hit_Writeback_Inv_SD
		if (a == end) break;
		a += sc_lsize;
	}
}

static void sr7100_dma_cache_wback_inv_tc(unsigned long addr,
					  unsigned long size)
{
	unsigned long end, a;

	a = addr & ~(sc_lsize - 1);
	end = (addr + size) & ~(sc_lsize - 1);
	while (1) {
		flush_dcache_line(a);
		flush_scache_line(a); // Hit_Writeback_Inv_SD
		flush_tcache_line(a); // Hit_Invalidate_T
		if (a == end) break;
		a += sc_lsize;
	}
}

/* It is kind of silly to writeback for the inv case.. Oh well */
static void sr7100_dma_cache_inv_sc(unsigned long addr, unsigned long size)
{
	unsigned long end, a;

	if (size >= scache_size) {
		sr7100_nuke_caches();
		return;
	}

	a = addr & ~(sc_lsize - 1);
	end = (addr + size) & ~(sc_lsize - 1);
	while (1) {
		flush_dcache_line(a);
		flush_scache_line(a); // Hit_Writeback_Inv_SD 
		if (a == end) break;
		a += sc_lsize;
	}
}

static void sr7100_dma_cache_inv_tc(unsigned long addr, unsigned long size)
{
	unsigned long end, a;

	a = addr & ~(sc_lsize - 1);
	end = (addr + size) & ~(sc_lsize - 1);
	while (1) {
		flush_dcache_line(a);
		flush_scache_line(a); // Hit_Writeback_Inv_SD
		flush_tcache_line(a); // Hit_Invalidate_T
		if (a == end) break;
		a += sc_lsize;
	}
}

static void sr7100_dma_cache_wback(unsigned long addr, unsigned long size)
{
	panic("sr7100_dma_cache_wback called - should not happen.");
}

/*
 * While we're protected against bad userland addresses we don't care
 * very much about what happens in that case.  Usually a segmentation
 * fault will dump the process later on anyway ...
 */
static void sr7100_flush_cache_sigtramp(unsigned long addr)
{
	protected_writeback_dcache_line(addr & ~(dc_lsize - 1));
	protected_flush_icache_line(addr & ~(ic_lsize - 1));
}

/* Detect and size the various caches. */
static void __init probe_icache(unsigned long config,unsigned long config1)
{
	unsigned int lsize;

	config1 = read_mips32_cp0_config1(); 
	
	if ((lsize = ((config1 >> 19) & 7)))
		mips_cpu.icache.linesz = 2 << lsize;
	else 
		mips_cpu.icache.linesz = lsize;
	mips_cpu.icache.sets = 64 << ((config1 >> 22) & 7);
	mips_cpu.icache.ways = 1 + ((config1 >> 16) & 7);
	
	ic_lsize = mips_cpu.icache.linesz;
	icache_size = mips_cpu.icache.sets * mips_cpu.icache.ways * 
		ic_lsize;
	printk("Primary instruction cache %dkb, linesize %d bytes (%d ways)\n",
	       icache_size >> 10, ic_lsize, mips_cpu.icache.ways);
}

static void __init probe_dcache(unsigned long config,unsigned long config1)
{
	unsigned int lsize;

	if ((lsize = ((config1 >> 10) & 7)))
		mips_cpu.dcache.linesz = 2 << lsize;
	else 
		mips_cpu.dcache.linesz = lsize;
	mips_cpu.dcache.sets = 64 << ((config1 >> 13) & 7);
	mips_cpu.dcache.ways = 1 + ((config1 >> 7) & 7);
	
	dc_lsize = mips_cpu.dcache.linesz;
	dcache_size = 
		mips_cpu.dcache.sets * mips_cpu.dcache.ways
		* dc_lsize;
	printk("Primary data cache %dkb, linesize %d bytes (%d ways)\n",
	       dcache_size >> 10, dc_lsize, mips_cpu.dcache.ways);
}

static void __init probe_scache(unsigned long config,unsigned long config2)
{
	unsigned int lsize;

	if ((lsize = ((config2 >> 4) & 7)))
		mips_cpu.scache.linesz = 2 << lsize;
	else 
		mips_cpu.scache.linesz = lsize;
	
	mips_cpu.scache.sets = 64 << ((config2 >> 8) & 7);
	mips_cpu.scache.ways = 1 + ((config2 >> 0) & 7);

	sc_lsize = mips_cpu.scache.linesz;
	scache_size = mips_cpu.scache.sets * mips_cpu.scache.ways * sc_lsize;
	
	printk("Secondary cache %dK, linesize %d bytes (%d ways)\n",
	       scache_size >> 10, sc_lsize, mips_cpu.scache.ways);
}

static void __init probe_tcache(unsigned long config,unsigned long config2)
{
	unsigned int lsize;

	/* Firmware or prom_init is required to configure the size of the 
	   tertiary cache in config2 and set the TE bit in config2 to signal 
	   the external SRAM chips are present. */
	if ((config2 & (1<<28)) == 0)
		return;
	
	if ((lsize = ((config2 >> 20) & 7)))
		mips_cpu.tcache.linesz = 2 << lsize;
	else 
		mips_cpu.tcache.linesz = lsize;
	
	mips_cpu.tcache.sets = 64 << ((config2 >> 24) & 7);
	mips_cpu.tcache.ways = 1 + ((config2 >> 16) & 7);

	tc_lsize = mips_cpu.tcache.linesz;
	tcache_size = mips_cpu.tcache.sets * mips_cpu.tcache.ways * tc_lsize;
	
	printk("Tertiary cache %dK, linesize %d bytes, blocksize %d "
	       "bytes (%d ways)\n",
	       tcache_size >> 10, sc_lsize, tc_lsize, mips_cpu.tcache.ways);
}

static void __init setup_scache_funcs(void)
{
        _flush_cache_all = sr7100_flush_cache_all_pc;
        ___flush_cache_all = sr7100_nuke_caches;
	_flush_cache_mm = sr7100_flush_cache_mm_pc;
	_flush_cache_range = sr7100_flush_cache_range_pc;
	_flush_cache_page = sr7100_flush_cache_page_pc;
	_flush_page_to_ram = sr7100_flush_page_to_ram_pc;
	_clear_page = (void *)mips32_clear_page_sc;
	_copy_page = (void *)mips32_copy_page_sc;

	_flush_icache_page = sr7100_flush_icache_page;

	if (tcache_size == 0)
	{
		_dma_cache_wback_inv = sr7100_dma_cache_wback_inv_sc;
		_dma_cache_inv = sr7100_dma_cache_inv_sc;
	}
	else
	{
		_dma_cache_wback_inv = sr7100_dma_cache_wback_inv_tc;
		_dma_cache_inv = sr7100_dma_cache_inv_tc;
	}
		
	_dma_cache_wback = sr7100_dma_cache_wback;
}

/* This implements the cache intialization stuff from the SR7100 guide. After
   this all the caches will be empty and ready to run. It must be run from
   uncached space. */
static void __init clear_enable_caches(unsigned long config)
{
	config = (config & (~CONF_CM_CMASK)) | CONF_CM_CACHABLE_NONCOHERENT;
	
	/* Primary cache init (7.1.1)
	   SR71000 Primary Cache initialization of 4-way, 32 Kbyte line I/D 
	   caches. */
	__asm__ __volatile__ 
		(
		 ".set push\n"
		 ".set noreorder\n"
		 ".set noat\n"
		 ".set mips64\n"
		 
		 // Enable KSEG0 caching
		 " mtc0 %0, $16\n"
		 
		 /* It is recommended that parity be disabled during cache 
		    initialization. */
		 " mfc0 $1, $12\n"      // Read CP0 Status Register.
		 " li $2, 0x00010000\n" // DE Bit.
		 " or $2, $1, $2\n"
		 " mtc0 $2, $12\n"     // Disable Parity.
		 
		 " ori $3, %1, 0x1FE0\n" // 256 sets.
		 " mtc0 $0, $28\n" // Set CP0 Tag_Lo Register
		 "1:\n"
		 " cache 8, 0x0000($3)\n" // Index_Store_Tag_I
		 " cache 8, 0x2000($3)\n" // Index_Store_Tag_I
		 " cache 8, 0x4000($3)\n" // Index_Store_Tag_I
		 " cache 8, 0x6000($3)\n" // Index_Store_Tag_I
		 " cache 9, 0x0000($3)\n" // Index_Store_Tag_D
		 " cache 9, 0x2000($3)\n" // Index_Store_Tag_D
		 " cache 9, 0x4000($3)\n" // Index_Store_Tag_D
		 " cache 9, 0x6000($3)\n" // Index_Store_Tag_D
		 " bne $3, %1, 1b\n"
		 " addiu $3, $3, -0x0020\n" // 32 byte cache line
		 " mtc0 $1, $12\n" // Put original back in Status Register.
		 ".set pop\n"
		 :
		 : "r"(config), "r"(KSEG0) 
		 : "$1","$2","$3");
		 // Fixme: use settings from config register not hardwired
		 
	/* Secondary and tertiary flash invalidate (7.5.18)	   
	    This code fragment, invalidates (also disables), and
	    restores (re-enables) the secondary and tertiary caches.
	    Ensure system is operating in uncached space. */
	__asm__ __volatile__ 
		 (
		 ".set push\n"
		 ".set noreorder\n"
		 ".set noat\n"
		 ".set mips64\n"
		 "  sync\n"   // flush core pipeline
		 "  lw $2, 0(%0)\n"             // flush pending accesses
		 "  bne $2, $2, 1f\n"           // prevent I-fetches
		 "  nop\n"
		 "1: mfc0 $1, $16, 2\n"        // save current Config2
		 "  li $2, 0x20002000\n"        // set flash invalidation bits
		 "  or $2, $1, $2\n"
		 "  mtc0 $2, $16, 2\n" // invalidate & disable caches
		 "  mtc0 $1, $16, 2\n" // restore Config2
		 ".set pop\n"
		 :
		 : "r"(KSEG1)
		 : "$1","$2");		 
}

void __init ld_mmu_sr7100(void)
{
	unsigned long config = read_32bit_cp0_registerx(CP0_CONFIG,0);
	unsigned long config1 = read_32bit_cp0_registerx(CP0_CONFIG,1);
	unsigned long config2 = read_32bit_cp0_registerx(CP0_CONFIG,2);
	void (*kseg1_cec)(unsigned long config) = (void *)KSEG1ADDR(&clear_enable_caches);

	// Should never happen
        if (!(config & (1 << 31)) || !(config1 & (1 << 31)))
		panic("sr7100 does not have necessary config registers");

	/* We should be uncached for this.. If the firmware has enabled
	   the cache it may have dirty data and we really need to flush
	   it before doing a mass invalidate, on the other hand if the
	   cache has not been inited flushing it could corrupt ram.. */
	if ((config & CONF_CM_CMASK) != CONF_CM_UNCACHED)
	{
		printk("Turning off cache.\n");
		change_cp0_config(CONF_CM_CMASK,CONF_CM_UNCACHED);
		
		// flush core pipeline
		__asm__ __volatile__ ("  sync\n");
		
		sr7100_flush_cache_all_pc();
		
		// Was the secondary cached turned on?
		if ((config2 & (1<<12)) != 0)
			blast_scache();
		
		// Tertiary is write through so it is safe
	}	
	
	probe_icache(config,config1);
	probe_dcache(config,config1);
	probe_scache(config,config2);
	probe_tcache(config,config2);
	setup_scache_funcs();

	// Make sure the the secondary cache is turned on (always present)
	write_32bit_cp0_registerx(CP0_CONFIG,2,config2 | (1<<12));
	
#ifndef CONFIG_MIPS_UNCACHED
	kseg1_cec(config);
#endif
						  
	_flush_cache_sigtramp = sr7100_flush_cache_sigtramp;
	_flush_icache_range = sr7100_flush_icache_range;
}
