#include <linux/elf.h>
#include <stdio.h>
#include <malloc.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <byteswap.h>

#define DEBUG 0

FILE *log = NULL;

enum
{
    INFO = 1, LOAD = 2
};

#if DEBUG
#define debug_printf printf
#define OUT(a,b,c) {}
#else
//#define debug_printf(fmt,a...) fprintf(log,fmt,##a)
#define debug_printf(fmt,a...) {}
#define OUT write
#endif

static int vmem_sub(int fd, caddr_t addr, int nbytes, long *byte_count)
{
	int n = 4096;
	caddr_t end_addr;
	char buf[4096];

	for (end_addr = addr + nbytes;
	     addr < end_addr;
	     addr += n) {
		if (end_addr < addr + n) {
			n = end_addr - addr;
		}
		if (read(fd, &buf, n) != n) {
			debug_printf("read segment error.\n");
			return -1;
		}
		OUT(1, &buf, n);
		*byte_count += n;
	}

	return 0;
}

static int scanfile(int fd, caddr_t *start, caddr_t *end, caddr_t *entry, int load)
{
	Elf32_Ehdr elfx, *elf = &elfx;
	int i, first;
	long byte_count;
	int progress;
	Elf32_Phdr *phtbl = NULL;
	caddr_t min_addr, max_addr;

	if (read(fd, (void*)elf, sizeof(Elf32_Ehdr)) != sizeof(Elf32_Ehdr)) {
		debug_printf("read header error\n");
		goto error_cleanup;
	}

	if (load)
		OUT(1, elf, sizeof(Elf32_Ehdr));

	if ((phtbl = (Elf32_Phdr *)malloc(sizeof(*phtbl) * elf->e_phnum)) == NULL) {
		debug_printf("alloc() error\n");
		goto error_cleanup;
	}

	if (lseek(fd, elf->e_phoff, SEEK_SET) == -1)  {
		debug_printf("seek for program header table error\n");
		goto error_cleanup;
	}

	if (read(fd, (void *)phtbl, sizeof(Elf32_Phdr) * elf->e_phnum)
				!= (int)(sizeof(Elf32_Phdr) * elf->e_phnum)) {
		debug_printf("read program header table error\n");
		goto error_cleanup;
	}

	if (load)
		OUT(1, phtbl, sizeof(Elf32_Phdr) * elf->e_phnum);

	/*
	 *  scan program header table
	 */
	first = 1;
	byte_count = 0;
	progress = 0;
	for (i = 0; i < elf->e_phnum; i++) {
		if (phtbl[i].p_type != PT_LOAD) {
			continue;
		}

		if (first || max_addr < (caddr_t)(phtbl[i].p_vaddr + phtbl[i].p_memsz)) {
			max_addr = (caddr_t)(phtbl[i].p_vaddr + phtbl[i].p_memsz);
		}
		if (first || (caddr_t)phtbl[i].p_vaddr < min_addr) {
			min_addr = (caddr_t)phtbl[i].p_vaddr;
		}

		if (load) {
			if (lseek(fd, phtbl[i].p_offset, SEEK_SET) == -1)  {
				debug_printf("seek for segment error\n");
				goto error_cleanup;
			}

			if (vmem_sub(fd,
				     (caddr_t)phtbl[i].p_vaddr,
				     phtbl[i].p_filesz,
				     &byte_count) != 0) {
				goto error_cleanup;
			}
		} else {
			byte_count += phtbl[i].p_memsz;
		}

		first = 0;
	}

	if (first) {
		debug_printf("can't find loadable segment\n");
		goto error_cleanup;
	}

	//debug_printf("entry=%x  addr=%x-%x\n",
	//	     elf->e_entry, min_addr, max_addr);

	if (phtbl) free(phtbl);

	if (start) *start = min_addr;
	if (end) *end = max_addr;
	if (entry) *entry = (caddr_t)elf->e_entry;
	return (0);

error_cleanup:
	if (phtbl) free(phtbl);
	return -1;
}


int main(int argc, char **argv)
{
    caddr_t start = NULL, end = NULL, entry = NULL;
    short int len;
    char fname[512];
    char path[512];
    char action = 0;
    FILE *fp = NULL;

    if (argc != 2)
	return -1;

    read(0, &len, 4);
    read(0, &fname, len);
    fname[len] = '\0';
    read(0, &action, 1);

    //log = fopen("/tmp/l4be.log", "a");
    //fprintf(log, "action=%s fname=%s\n", action == INFO ? "INFO" : "LOAD", fname);
    //fflush(log);
    sprintf(path, "%s/%s", argv[1], fname);
    fp = fopen(path, "r");
    if (!fp) {
	debug_printf("Unable to open file (%s): %s\n", path, strerror(errno));
	//fclose(log);
	return 0;
    }
    switch (action) {
	case INFO:
	    scanfile(fileno(fp), &start, &end, NULL, 0);
	    if (start && end) {
		OUT(1, &start, sizeof(start));
		OUT(1, &end, sizeof(end));
	    }
	    break;
	case LOAD:
	    scanfile(fileno(fp), NULL, NULL, &entry, 1);
	    if (entry)
		OUT(1, &entry, sizeof(entry));
	    break;
    }
    //fclose(log);
    return 0;
}
