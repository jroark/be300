// pbsdboot.h
// $Id: pbsdboot.h,v 1.3 2004/08/28 21:29:33 be300 Exp $

#ifdef __cplusplus
extern "C" {
#endif

#include <windows.h>
#include <winsock.h>

BOOL VirtualCopy(LPVOID, LPVOID, DWORD, DWORD);

typedef char *caddr_t;							// From kernel code
typedef unsigned int __SIZE_TYPE__;				// From gcc compiler
typedef unsigned int __SSIZE_TYPE__;			// From gcc compiler
#define __signed__ signed						// More hacks against gcc
#define __inline__ __inline						// More hacks against gcc
#define __builtin_constant_p(x) 0				// More hacks against gcc

#include <linux/types.h>
#include <linux/elf.h>

#include "../lib/fileops.h"
#include "../lib/debug.h"

// convert from BSD to Linux
#define Elf_Ehdr Elf32_Ehdr
#define Elf_Shdr Elf32_Shdr
#define Elf_Phdr Elf32_Phdr
#define Elf_pt_load PT_LOAD
#define Elf_sht_symtab SHT_SYMTAB
#define Elf_sht_strtab SHT_STRTAB
#define alloc malloc
#define free(a, b) free(a)

#define PATHBUFLEN 200
#define whoami _T("Error")

#define CYACE_PROBE_MAGIC              0x43595042u /* "CYPB" */
#define CYACE_PROBE_VERSION            1u
#define CYACE_PROBE_RAM_WORD_COUNT     8u
#define CYACE_PROBE_MMIO_WORD_COUNT    16u
#define CYACE_PROBE_CWIN_WORD_COUNT    20u
#define CYACE_PROBE_BLOCK_SIZE         0x400u

/* Fixed offsets used by the final kernel-mode handoff stub. */
#define CYACE_PROBE_OFF_KERNEL_STAMP_READY 0x18u
#define CYACE_PROBE_OFF_KSTATUS            0x20u
#define CYACE_PROBE_OFF_KCONFIG            0x24u
#define CYACE_PROBE_OFF_KWIRED             0x28u
#define CYACE_PROBE_OFF_KCOUNT             0x2Cu
#define CYACE_PROBE_OFF_KCOMPARE           0x30u
#define CYACE_PROBE_OFF_KSP                0x34u
#define CYACE_PROBE_OFF_KRA                0x38u
#define CYACE_PROBE_OFF_KENTRY             0x3Cu

struct cyace_probe_block_s {
  unsigned long magic;
  unsigned long version;
  unsigned long size_bytes;
  unsigned long crc32;
  unsigned long flags;
  unsigned long preexec_ready;
  unsigned long reserved0;
  unsigned long reserved1;
  unsigned long kernel_stamp_ready;
  unsigned long kernel_status;
  unsigned long kernel_config;
  unsigned long kernel_wired;
  unsigned long kernel_count;
  unsigned long kernel_compare;
  unsigned long kernel_sp;
  unsigned long kernel_ra;
  unsigned long kernel_entry;
  unsigned long entry_pa;
  unsigned long argc;
  unsigned long argv_pa;
  unsigned long bootinfo_pa;
  unsigned long map_pa;
  unsigned long map_base_pa;
  unsigned long pagesize;
  unsigned long leafsize;
  unsigned long nleaves;
  unsigned long first_leaf_pa;
  unsigned long last_leaf_pa;
  unsigned long probe_pa;
  unsigned long ram_word_pa[CYACE_PROBE_RAM_WORD_COUNT];
  unsigned long ram_word_val[CYACE_PROBE_RAM_WORD_COUNT];
  unsigned long ram_word_ok_mask;
  unsigned long mmio_word_pa[CYACE_PROBE_MMIO_WORD_COUNT];
  unsigned long mmio_word_val[CYACE_PROBE_MMIO_WORD_COUNT];
  unsigned long mmio_word_ok_mask;
  unsigned long cwin_word_val[CYACE_PROBE_CWIN_WORD_COUNT];
  unsigned long cwin_ok_mask;
  unsigned long crc_0f_page;
  unsigned long crc_0a_cwin;
  unsigned long cwin_base_pa;
  unsigned long cwin_len_bytes;
  unsigned long io_base_pa;
  unsigned long io_len_bytes;
  unsigned long reserved_tail[150];
};
typedef char cyace_probe_block_size_check[
  (sizeof(struct cyace_probe_block_s) == CYACE_PROBE_BLOCK_SIZE) ? 1 : -1
];

struct map_s {
  caddr_t entry;
  caddr_t base;
  int pagesize;
  int leafsize;
  int nleaves;
  caddr_t arg0;
  caddr_t arg1;
  caddr_t arg2;
  caddr_t arg3;
  caddr_t *leaf[32];
};

struct preference_s {
	int setting_idx;
	int fb_type;
	int fb_width, fb_height, fb_linebytes;
	long fb_addr;
	unsigned long platid_cpu, platid_machine;
	TCHAR setting_name[PATHBUFLEN];
	TCHAR kernel_name[PATHBUFLEN];
	TCHAR options[PATHBUFLEN];
	BOOL check_last_chance;
	BOOL load_debug_info;
	BOOL from_network;
	BOOL serial_port;
};

extern struct preference_s pref;

// virtual address of start of physical memory
extern unsigned int phys_start;

// final phase function
typedef int (*startprog_t)(caddr_t map);
extern startprog_t startprog;

// progress feedback function
typedef BOOL (*CheckCancel_t)(int progress);
extern CheckCancel_t CheckCancel;

// vmem.c
int vmem_exec(caddr_t entry, int argc, char *argv[], struct bootinfo *bi);
DWORD getpagesize(void);
caddr_t vmem_get(caddr_t phys_addr, int *length);
int vmem_init(caddr_t start, caddr_t end);
void vmem_dump_map(void);
caddr_t vtophysaddr(caddr_t page);
void vmem_free(void);
caddr_t vmem_alloc(void);
int vmem_probe_prepare(char *arg, int arglen);
caddr_t vmem_probe_pa(void);

// elf.c
int getinfo(int fd, caddr_t *start, caddr_t *end);
int loadfile(int fd, caddr_t *entry);
int getinfo2(struct sockaddr_in *sin, const char *url, const char *image, caddr_t *start, caddr_t *end);
int loadfile2(struct sockaddr_in *sin, const char *url, const char *image, caddr_t *entry);

#ifdef __cplusplus
}
#endif
