#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "loader.h"
#include "machine.h"

/* ------------------------------------------------------------------ */
/* Minimal ELF32 structures (little-endian)                             */
/* ------------------------------------------------------------------ */

#define ELF_MAGIC       "\x7f" "ELF"
#define ET_EXEC         2
#define EM_MIPS         8
#define ELFCLASS32      1
#define ELFDATA2LSB     1   /* little-endian */
#define PT_LOAD         1

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed)) Elf32_Phdr;

static int load_file(machine_t *m, const char *path,
                     uint64_t pa, uint64_t region_size,
                     const char *label)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[LOADER] Cannot open %s: %s\n", path, label);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);

    if (fsize <= 0) {
        fprintf(stderr, "[LOADER] %s is empty\n", label);
        fclose(f);
        return -1;
    }
    if ((uint64_t)fsize > region_size) {
        fprintf(stderr, "[LOADER] %s too large (%ld bytes, region %llu bytes)\n",
                label, fsize, (unsigned long long)region_size);
        fclose(f);
        return -1;
    }

    void *buf = malloc((size_t)fsize);
    if (!buf) {
        fprintf(stderr, "[LOADER] OOM reading %s\n", label);
        fclose(f);
        return -1;
    }

    if ((long)fread(buf, 1, (size_t)fsize, f) != fsize) {
        fprintf(stderr, "[LOADER] Short read from %s\n", label);
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    uc_err err = uc_mem_write(m->uc, pa, buf, (size_t)fsize);
    free(buf);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "[LOADER] uc_mem_write @ 0x%08llX failed: %s\n",
                (unsigned long long)pa, uc_strerror(err));
        return -1;
    }

    fprintf(stderr, "[LOADER] Loaded %ld bytes from '%s' to PA 0x%08llX\n",
            fsize, path, (unsigned long long)pa);
    return 0;
}

int loader_load_rom(machine_t *m, const char *path)
{
    /*
     * The ROM image is loaded at PA_RESET_VECTOR (0x1FC00000), which is
     * the MIPS reset vector physical address inside the ROM region.
     * Maximum loadable size is the remainder of the ROM region.
     */
    uint64_t max = PA_ROM_BASE + PA_ROM_SIZE - PA_RESET_VECTOR;
    return load_file(m, path, PA_RESET_VECTOR, max, "ROM");
}

int loader_load_ram(machine_t *m, const char *path)
{
    return load_file(m, path, PA_SDRAM_BASE, m->cfg.sdram_size, "RAM");
}

int loader_load_elf(machine_t *m, const char *path, uint32_t *entry_va_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[LOADER] Cannot open ELF: %s\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);

    uint8_t *data = malloc((size_t)fsize);
    if (!data) {
        fprintf(stderr, "[LOADER] OOM reading ELF (%ld bytes)\n", fsize);
        fclose(f);
        return -1;
    }
    if ((long)fread(data, 1, (size_t)fsize, f) != fsize) {
        fprintf(stderr, "[LOADER] Short read from ELF\n");
        free(data); fclose(f); return -1;
    }
    fclose(f);

    /* Validate ELF header */
    if (fsize < (long)sizeof(Elf32_Ehdr)) {
        fprintf(stderr, "[LOADER] File too small to be ELF32\n");
        free(data); return -1;
    }
    Elf32_Ehdr *eh = (Elf32_Ehdr *)data;
    if (memcmp(eh->e_ident, ELF_MAGIC, 4) != 0) {
        fprintf(stderr, "[LOADER] Not an ELF file\n");
        free(data); return -1;
    }
    if (eh->e_ident[4] != ELFCLASS32) {
        fprintf(stderr, "[LOADER] Not ELF32 (class=%u)\n", eh->e_ident[4]);
        free(data); return -1;
    }
    if (eh->e_ident[5] != ELFDATA2LSB) {
        fprintf(stderr, "[LOADER] Not little-endian ELF\n");
        free(data); return -1;
    }
    if (eh->e_machine != EM_MIPS) {
        fprintf(stderr, "[LOADER] Not MIPS ELF (machine=%u)\n", eh->e_machine);
        free(data); return -1;
    }

    fprintf(stderr, "[LOADER] ELF32 MIPS LE  entry=0x%08X  phnum=%u\n",
            eh->e_entry, eh->e_phnum);

    /* Load PT_LOAD segments */
    for (int i = 0; i < eh->e_phnum; i++) {
        if (eh->e_phoff + (i + 1) * eh->e_phentsize > (uint32_t)fsize) {
            fprintf(stderr, "[LOADER] Program header %d out of file bounds\n", i);
            free(data); return -1;
        }
        Elf32_Phdr *ph = (Elf32_Phdr *)(data + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;

        /*
         * Physical address: strip kseg0/kseg1 prefix bits (top 3) to get
         * the hardware physical address.  Both 0x80... (kseg0) and
         * 0xA0... (kseg1) paddr values map to the same PA.
         */
        uint32_t pa     = ph->p_paddr & 0x1FFFFFFFu;
        uint32_t filesz = ph->p_filesz;
        uint32_t memsz  = ph->p_memsz;

        fprintf(stderr, "[LOADER]   seg[%d] PA=0x%08X  filesz=0x%X  memsz=0x%X\n",
                i, pa, filesz, memsz);

        if (pa + memsz > m->cfg.sdram_size) {
            fprintf(stderr, "[LOADER] Segment %d exceeds SDRAM (PA=0x%X memsz=0x%X sdram=%u)\n",
                    i, pa, memsz, m->cfg.sdram_size);
            free(data); return -1;
        }

        /* Copy file image */
        if (filesz > 0) {
            if (ph->p_offset + filesz > (uint32_t)fsize) {
                fprintf(stderr, "[LOADER] Segment %d data out of file bounds\n", i);
                free(data); return -1;
            }
            uc_err err = uc_mem_write(m->uc, pa, data + ph->p_offset, filesz);
            if (err != UC_ERR_OK) {
                fprintf(stderr, "[LOADER] uc_mem_write seg[%d] @ PA 0x%08X: %s\n",
                        i, pa, uc_strerror(err));
                free(data); return -1;
            }
        }

        /* Zero BSS region */
        if (memsz > filesz) {
            uint32_t bss_pa  = pa + filesz;
            uint32_t bss_len = memsz - filesz;
            uint8_t *zeros   = calloc(1, bss_len);
            if (zeros) {
                uc_mem_write(m->uc, bss_pa, zeros, bss_len);
                free(zeros);
            }
            fprintf(stderr, "[LOADER]   zeroed BSS PA=0x%08X len=0x%X\n",
                    bss_pa, bss_len);
        }
    }

    if (entry_va_out)
        *entry_va_out = eh->e_entry;

    free(data);
    fprintf(stderr, "[LOADER] ELF loaded, entry VA=0x%08X\n", eh->e_entry);
    return 0;
}
