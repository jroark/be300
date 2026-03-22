#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "loader.h"
#include "be300.h"

/* GXemul memory API */
#include "cpu.h"
#include "memory.h"

/* ------------------------------------------------------------------ */
/* Minimal ELF32 structures (little-endian)                             */
/* ------------------------------------------------------------------ */

#define ELF_MAGIC       "\x7f" "ELF"
#define ET_EXEC         2
#define EM_MIPS         8
#define ELFCLASS32      1
#define ELFDATA2LSB     1
#define PT_LOAD         1
#define SHT_SYMTAB      2
#define SHT_DYNSYM      11

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

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} __attribute__((packed)) Elf32_Shdr;

typedef struct {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
} __attribute__((packed)) Elf32_Sym;

/*
 * Write data to GXemul's emulated physical memory.
 * Uses kseg0 address (0x80000000 | pa) to bypass TLB.
 */
static void gxe_mem_write(struct cpu *cpu, uint32_t pa,
                           const void *buf, size_t len)
{
    uint64_t kseg0_addr = 0xffffffff80000000ULL | (uint64_t)pa;
    store_buf(cpu, kseg0_addr, (const char *)buf, len);
}

/*
 * Zero a region of GXemul's emulated physical memory.
 */
static void gxe_mem_zero(struct cpu *cpu, uint32_t pa, size_t len)
{
    uint8_t *zeros = calloc(1, len);
    if (zeros) {
        gxe_mem_write(cpu, pa, zeros, len);
        free(zeros);
    }
}

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

    gxe_mem_write(m->cpu, (uint32_t)pa, buf, (size_t)fsize);
    free(buf);

    fprintf(stderr, "[LOADER] Loaded %ld bytes from '%s' to PA 0x%08llX\n",
            fsize, path, (unsigned long long)pa);
    return 0;
}

int loader_load_rom(machine_t *m, const char *path)
{
    uint64_t max = PA_ROM_BASE + PA_ROM_SIZE - PA_RESET_VECTOR;
    return load_file(m, path, PA_RESET_VECTOR, max, "ROM");
}

int loader_load_ram(machine_t *m, const char *path)
{
    return load_file(m, path, PA_SDRAM_BASE, m->cfg.sdram_size, "RAM");
}

static bool find_jiffies_symbol_pa(const uint8_t *data, long fsize,
                                   const Elf32_Ehdr *eh,
                                   uint32_t *jiffies_pa_out)
{
    if (!jiffies_pa_out)
        return false;
    *jiffies_pa_out = 0;

    if (eh->e_shoff == 0 || eh->e_shnum == 0 || eh->e_shentsize < sizeof(Elf32_Shdr))
        return false;
    if (eh->e_shoff + (uint32_t)eh->e_shnum * eh->e_shentsize > (uint32_t)fsize)
        return false;

    const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(data + eh->e_shoff);
    uint32_t fallback_pa = 0;
    bool have_fallback = false;

    for (uint32_t i = 0; i < eh->e_shnum; i++) {
        const Elf32_Shdr *sym_sh = (const Elf32_Shdr *)((const uint8_t *)shdrs + i * eh->e_shentsize);
        if (sym_sh->sh_type != SHT_SYMTAB && sym_sh->sh_type != SHT_DYNSYM)
            continue;
        if (sym_sh->sh_entsize < sizeof(Elf32_Sym) || sym_sh->sh_size < sym_sh->sh_entsize)
            continue;
        if (sym_sh->sh_offset + sym_sh->sh_size > (uint32_t)fsize)
            continue;
        if (sym_sh->sh_link >= eh->e_shnum)
            continue;

        const Elf32_Shdr *str_sh = (const Elf32_Shdr *)((const uint8_t *)shdrs + sym_sh->sh_link * eh->e_shentsize);
        if (str_sh->sh_offset + str_sh->sh_size > (uint32_t)fsize || str_sh->sh_size == 0)
            continue;

        const uint8_t *strtab = data + str_sh->sh_offset;
        const Elf32_Sym *syms = (const Elf32_Sym *)(data + sym_sh->sh_offset);
        uint32_t sym_count = sym_sh->sh_size / sym_sh->sh_entsize;

        for (uint32_t s = 0; s < sym_count; s++) {
            const Elf32_Sym *sym = (const Elf32_Sym *)((const uint8_t *)syms + s * sym_sh->sh_entsize);
            if (sym->st_name >= str_sh->sh_size)
                continue;
            const char *name = (const char *)(strtab + sym->st_name);
            uint32_t va = sym->st_value;
            uint32_t sym_pa = va & 0x1FFFFFFFu;
            if (strcmp(name, "jiffies") == 0) {
                *jiffies_pa_out = sym_pa;
                return true;
            }
            if (!have_fallback && strcmp(name, "jiffies_64") == 0) {
                fallback_pa = sym_pa;
                have_fallback = true;
            }
        }
    }

    if (have_fallback) {
        *jiffies_pa_out = fallback_pa;
        return true;
    }
    return false;
}

int loader_load_elf(machine_t *m, const char *path,
                    uint32_t *entry_va_out,
                    uint32_t *jiffies_pa_out)
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

        if (filesz > 0) {
            if (ph->p_offset + filesz > (uint32_t)fsize) {
                fprintf(stderr, "[LOADER] Segment %d data out of file bounds\n", i);
                free(data); return -1;
            }
            gxe_mem_write(m->cpu, pa, data + ph->p_offset, filesz);
        }

        if (memsz > filesz) {
            uint32_t bss_pa  = pa + filesz;
            uint32_t bss_len = memsz - filesz;
            gxe_mem_zero(m->cpu, bss_pa, bss_len);
            fprintf(stderr, "[LOADER]   zeroed BSS PA=0x%08X len=0x%X\n",
                    bss_pa, bss_len);
        }
    }

    uint32_t entry_va = eh->e_entry;
    if (entry_va_out)
        *entry_va_out = entry_va;

    if (jiffies_pa_out) {
        uint32_t jiffies_pa = 0;
        if (find_jiffies_symbol_pa(data, fsize, eh, &jiffies_pa)) {
            *jiffies_pa_out = jiffies_pa;
            fprintf(stderr, "[LOADER] Symbol jiffies PA=0x%08X\n", jiffies_pa);
        } else {
            *jiffies_pa_out = 0;
            fprintf(stderr, "[LOADER] Symbol jiffies not found in ELF symbols\n");
        }
    }

    free(data);
    fprintf(stderr, "[LOADER] ELF loaded, entry VA=0x%08X\n", entry_va);
    return 0;
}

/* ------------------------------------------------------------------ */
/* WinCE NAND / B000FF loader                                          */
/* ------------------------------------------------------------------ */

#define B000FF_SIG        "B000FF\n"
#define B000FF_SIG_LEN    7
#define NAND_SPL_OFFSET   0x4000u

int loader_load_nand(machine_t *m, const char *path,
                     uint32_t *entry_va_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[LOADER] Cannot open NAND image: %s\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);

    if (fsize < (long)(NAND_SPL_OFFSET + B000FF_SIG_LEN + 8 + 12)) {
        fprintf(stderr, "[LOADER] NAND image too small (%ld bytes)\n", fsize);
        fclose(f);
        return -1;
    }

    uint8_t *data = malloc((size_t)fsize);
    if (!data) {
        fprintf(stderr, "[LOADER] OOM reading NAND (%ld bytes)\n", fsize);
        fclose(f);
        return -1;
    }
    if ((long)fread(data, 1, (size_t)fsize, f) != fsize) {
        fprintf(stderr, "[LOADER] Short read from NAND image\n");
        free(data); fclose(f); return -1;
    }
    fclose(f);

    /* Validate B000FF signature */
    if (memcmp(data + NAND_SPL_OFFSET, B000FF_SIG, B000FF_SIG_LEN) != 0) {
        fprintf(stderr, "[LOADER] No B000FF signature at NAND offset 0x%X\n",
                NAND_SPL_OFFSET);
        free(data);
        return -1;
    }

    uint32_t off = NAND_SPL_OFFSET + B000FF_SIG_LEN;
    uint32_t img_start, img_length;
    memcpy(&img_start,  data + off, 4); off += 4;
    memcpy(&img_length, data + off, 4); off += 4;

    fprintf(stderr, "[LOADER] B000FF: image_start=0x%08X  image_length=0x%X\n",
            img_start, img_length);

    uint32_t records_start = off;
    uint32_t records_end   = records_start + img_length;
    if (records_end > (uint32_t)fsize) {
        fprintf(stderr, "[LOADER] B000FF image extends past NAND\n");
        free(data);
        return -1;
    }

    int rec_count = 0;
    while (off + 12 <= records_end) {
        uint32_t addr, length, cksum;
        uint32_t rec_hdr_off = off;
        memcpy(&addr,   data + off, 4);
        memcpy(&length, data + off + 4, 4);
        memcpy(&cksum,  data + off + 8, 4);
        off += 12;

        if (length == 0) {
            fprintf(stderr, "[LOADER] B000FF: entry point VA=0x%08X (%d records loaded)\n",
                    addr, rec_count);
            if (entry_va_out)
                *entry_va_out = addr;
            m->nand_data = data;
            m->nand_size = (size_t)fsize;
            return 0;
        }

        if (off + length > records_end) {
            /* Check for 4-byte sentinel before entry record */
            if (addr == 0 && rec_hdr_off + 4 + 12 <= records_end) {
                uint32_t next_addr, next_len, next_cksum;
                uint32_t next_off = rec_hdr_off + 4;
                memcpy(&next_addr, data + next_off, 4);
                memcpy(&next_len,  data + next_off + 4, 4);
                memcpy(&next_cksum, data + next_off + 8, 4);
                bool next_is_kseg =
                    ((next_addr & 0xE0000000u) == 0x80000000u) ||
                    ((next_addr & 0xE0000000u) == 0xA0000000u);
                if (next_len == 0 && next_is_kseg && next_cksum == 0xFFFFFFFFu) {
                    fprintf(stderr, "[LOADER] B000FF: skipped sentinel, entry VA=0x%08X\n",
                            next_addr);
                    if (entry_va_out) *entry_va_out = next_addr;
                    m->nand_data = data;
                    m->nand_size = (size_t)fsize;
                    return 0;
                }
            }
            fprintf(stderr, "[LOADER] B000FF record %d data overflows\n", rec_count);
            break;
        }

        uint32_t pa = addr & 0x1FFFFFFFu;
        fprintf(stderr, "[LOADER] B000FF: rec[%d] VA=0x%08X PA=0x%08X len=0x%X\n",
                rec_count, addr, pa, length);

        if (pa + length <= m->cfg.sdram_size) {
            gxe_mem_write(m->cpu, pa, data + off, length);
        } else {
            fprintf(stderr, "[LOADER] B000FF: rec[%d] exceeds SDRAM, skipping\n", rec_count);
        }

        off += length;
        rec_count++;
    }

    fprintf(stderr, "[LOADER] B000FF: no entry record, using image_start=0x%08X\n",
            img_start);
    if (entry_va_out)
        *entry_va_out = img_start;
    m->nand_data = data;
    m->nand_size = (size_t)fsize;
    return 0;
}
