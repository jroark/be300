/*
 * Offline .cbea installer: see cbea_install.h and docs/INSTALLING_APPS.md.
 *
 * Package layout (tools/dumpcbea/main.c, checked against WMP 1.1, AIM and
 * REQ packages): u16 version, u32 file count at +2, UTF-16 target string at
 * +8, entries from +0x100 as
 *     u32 next, u8 kind[3], u8 name_len, name + NUL, u32 size, data[size]
 * The Install.inf entry drives the install.
 */

#include "cbea_install.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SECTOR_SIZE         512u
#define DIRENT_SIZE         32u
#define ATTR_DIR            0x10u
#define ATTR_VOLUME         0x08u
#define ATTR_LFN            0x0Fu
#define FAT16_EOC           0xFFFFu
#define LFN_CHARS           13u
#define MAX_NAME            255u
#define MAX_PATH_LEN        520u

#define REG_ROOT_HKCR       0u
#define REG_ROOT_HKCU       1u
#define REG_ROOT_HKLM       2u
#define REG_TYPE_SZ         1u
#define REG_TYPE_EXPAND_SZ  2u
#define REG_TYPE_BINARY     3u
#define REG_TYPE_DWORD      4u
#define REG_TYPE_MULTI_SZ   7u

static const uint8_t k_reg_magic[4] = { 0xB2, 0x74, 0x83, 0x1D };

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} logbuf_t;

static void log_add(logbuf_t *l, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (!l->buf || l->cap == 0 || l->len + 1 >= l->cap)
        return;
    va_start(ap, fmt);
    n = vsnprintf(l->buf + l->len, l->cap - l->len, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    l->len += (size_t)n;
    if (l->len >= l->cap)
        l->len = l->cap - 1;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int lower_ascii(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int upper_ascii(int c)
{
    return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;
}

static bool str_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (lower_ascii((unsigned char)*a) != lower_ascii((unsigned char)*b))
            return false;
        a++;
        b++;
    }
    return *a == *b;
}

static bool str_iprefix(const char *s, const char *prefix)
{
    while (*prefix) {
        if (lower_ascii((unsigned char)*s) != lower_ascii((unsigned char)*prefix))
            return false;
        s++;
        prefix++;
    }
    return true;
}

/* Names in packages are ANSI; treat bytes as Latin-1 code units. */
static bool name_eq_u16(const uint16_t *u, size_t ulen, const char *s)
{
    size_t slen = strlen(s);

    if (ulen != slen)
        return false;
    for (size_t i = 0; i < ulen; i++) {
        unsigned c = (unsigned char)s[i];
        if (u[i] > 0xFF || lower_ascii((int)u[i]) != lower_ascii((int)c))
            return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* FAT16 volume                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *base;
    size_t size;
    uint32_t spc;
    uint32_t nfats;
    uint32_t root_entries;
    uint32_t fat_sectors;
    uint32_t fat_off;
    uint32_t root_off;
    uint32_t data_off;
    uint32_t cluster_bytes;
    uint32_t cluster_count;
    uint32_t alloc_hint;
} fat_vol_t;

static bool fat_parse_bpb(uint8_t *base, size_t avail, fat_vol_t *v)
{
    uint32_t bps, reserved, total;
    uint64_t data_sectors;

    if (avail < SECTOR_SIZE)
        return false;
    if (base[0x1FE] != 0x55 || base[0x1FF] != 0xAA ||
        memcmp(base + 0x36, "FAT16", 5) != 0)
        return false;

    bps = rd16(base + 0x0B);
    memset(v, 0, sizeof(*v));
    v->spc = base[0x0D];
    reserved = rd16(base + 0x0E);
    v->nfats = base[0x10];
    v->root_entries = rd16(base + 0x11);
    total = rd16(base + 0x13);
    if (total == 0)
        total = rd32(base + 0x20);
    v->fat_sectors = rd16(base + 0x16);

    if (bps != SECTOR_SIZE || v->spc == 0 || (v->spc & (v->spc - 1)) != 0 ||
        reserved == 0 || v->nfats == 0 || v->fat_sectors == 0 ||
        (v->root_entries * DIRENT_SIZE) % SECTOR_SIZE != 0)
        return false;
    if ((uint64_t)total * SECTOR_SIZE > avail)
        return false;

    v->base = base;
    v->size = (size_t)total * SECTOR_SIZE;
    v->fat_off = reserved * SECTOR_SIZE;
    v->root_off = v->fat_off + v->nfats * v->fat_sectors * SECTOR_SIZE;
    v->data_off = v->root_off + v->root_entries * DIRENT_SIZE;
    v->cluster_bytes = v->spc * SECTOR_SIZE;
    if (v->data_off >= v->size)
        return false;
    data_sectors = (v->size - v->data_off) / SECTOR_SIZE;
    v->cluster_count = (uint32_t)(data_sectors / v->spc);
    if ((uint64_t)v->fat_sectors * SECTOR_SIZE / 2 < (uint64_t)v->cluster_count + 2)
        v->cluster_count = v->fat_sectors * SECTOR_SIZE / 2 - 2;
    v->alloc_hint = 2;
    return v->cluster_count > 0;
}

/*
 * Locate \Nand Disk. Restore images start with a block-0 partition table of
 * 16-byte entries (8 x 0xFF, u32 start sector, u32 sector count); entry 3 is
 * the FAT16 volume (CLAUDE.md "Restore image partition table"). Scan the
 * same way as nand_build_sector0_mbr() in src/hw/nand.c. A bare FAT16
 * volume at sector 0 is also accepted.
 */
static bool fat_locate(uint8_t *img, size_t len, fat_vol_t *v)
{
    if (fat_parse_bpb(img, len, v))
        return true;
    if (len < 0x40)
        return false;
    for (uint32_t off = 0; off + 8 <= 0x40u; off += 4) {
        uint32_t start = rd32(img + off);
        uint32_t count = rd32(img + off + 4);
        uint64_t start_b = (uint64_t)start * SECTOR_SIZE;
        uint64_t end_b = ((uint64_t)start + count) * SECTOR_SIZE;

        if (start == 0 || count == 0 || start == 0xFFFFFFFFu ||
            count == 0xFFFFFFFFu || end_b > len)
            continue;
        if (fat_parse_bpb(img + start_b, (size_t)(end_b - start_b), v))
            return true;
    }
    return false;
}

static uint32_t fat_get(const fat_vol_t *v, uint32_t c)
{
    return rd16(v->base + v->fat_off + c * 2u);
}

static void fat_set(fat_vol_t *v, uint32_t c, uint32_t val)
{
    for (uint32_t i = 0; i < v->nfats; i++)
        wr16(v->base + v->fat_off + i * v->fat_sectors * SECTOR_SIZE + c * 2u, val);
}

static bool fat_valid_cluster(const fat_vol_t *v, uint32_t c)
{
    return c >= 2 && c < v->cluster_count + 2;
}

static uint8_t *fat_cluster_ptr(const fat_vol_t *v, uint32_t c)
{
    return v->base + v->data_off + (c - 2u) * v->cluster_bytes;
}

static uint32_t fat_free_clusters(const fat_vol_t *v)
{
    uint32_t n = 0;

    for (uint32_t c = 2; c < v->cluster_count + 2; c++) {
        if (fat_get(v, c) == 0)
            n++;
    }
    return n;
}

/* Allocate one zeroed cluster marked end-of-chain; 0 when the disk is full. */
static uint32_t fat_alloc(fat_vol_t *v)
{
    uint32_t end = v->cluster_count + 2;

    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t c = pass == 0 ? v->alloc_hint : 2;
        uint32_t stop = pass == 0 ? end : v->alloc_hint;
        for (; c < stop; c++) {
            if (fat_get(v, c) == 0) {
                fat_set(v, c, FAT16_EOC);
                memset(fat_cluster_ptr(v, c), 0, v->cluster_bytes);
                v->alloc_hint = c + 1;
                return c;
            }
        }
    }
    return 0;
}

static void fat_free_chain(fat_vol_t *v, uint32_t c)
{
    uint32_t guard = v->cluster_count;

    while (fat_valid_cluster(v, c) && guard--) {
        uint32_t next = fat_get(v, c);
        fat_set(v, c, 0);
        if (c < v->alloc_hint)
            v->alloc_hint = c;
        c = next;
    }
}

/*
 * Directory slot access. A directory is named by its first cluster; 0 is
 * the fixed root region. With `extend`, a subdirectory grows by one zeroed
 * cluster when idx runs past its chain.
 */
static uint8_t *dir_slot(fat_vol_t *v, uint32_t dir, uint32_t idx, bool extend)
{
    uint32_t per = v->cluster_bytes / DIRENT_SIZE;
    uint32_t c = dir;
    uint32_t guard = v->cluster_count;

    if (dir == 0) {
        if (idx >= v->root_entries)
            return NULL;
        return v->base + v->root_off + idx * DIRENT_SIZE;
    }
    while (idx >= per) {
        uint32_t next = fat_get(v, c);
        if (!fat_valid_cluster(v, next)) {
            if (!extend)
                return NULL;
            next = fat_alloc(v);
            if (!next)
                return NULL;
            fat_set(v, c, next);
        }
        c = next;
        idx -= per;
        if (!guard--)
            return NULL;
    }
    return fat_cluster_ptr(v, c) + idx * DIRENT_SIZE;
}

static uint8_t lfn_checksum(const uint8_t short_name[11])
{
    uint8_t sum = 0;

    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + short_name[i]);
    return sum;
}

static const uint8_t k_lfn_offsets[LFN_CHARS] = {
    1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
};

static size_t short_to_display(const uint8_t *e, uint16_t *out)
{
    size_t n = 0;
    int base_end = 8, ext_end = 3;
    bool base_lower = (e[12] & 0x08) != 0;
    bool ext_lower = (e[12] & 0x10) != 0;

    while (base_end > 0 && e[base_end - 1] == ' ')
        base_end--;
    while (ext_end > 0 && e[8 + ext_end - 1] == ' ')
        ext_end--;
    for (int i = 0; i < base_end; i++) {
        uint8_t ch = (i == 0 && e[0] == 0x05) ? 0xE5 : e[i];
        out[n++] = (uint16_t)(base_lower ? lower_ascii(ch) : ch);
    }
    if (ext_end > 0) {
        out[n++] = '.';
        for (int i = 0; i < ext_end; i++)
            out[n++] = (uint16_t)(ext_lower ? lower_ascii(e[8 + i]) : e[8 + i]);
    }
    return n;
}

typedef struct {
    uint8_t *ent;       /* the 8.3 entry */
    uint32_t first;     /* first slot of the entry set (LFN or short) */
    uint32_t last;      /* slot of the 8.3 entry */
} dir_hit_t;

/* Find `name` (case-insensitive, long or short form) in a directory. */
static bool dir_find(fat_vol_t *v, uint32_t dir, const char *name, dir_hit_t *hit)
{
    uint16_t lfn[LFN_CHARS * 20];
    uint16_t disp[16];
    int expect = 0;
    uint8_t chk = 0;
    uint32_t lfn_first = 0;

    for (uint32_t idx = 0;; idx++) {
        uint8_t *e = dir_slot(v, dir, idx, false);
        if (!e || e[0] == 0x00)
            return false;
        if (e[0] == 0xE5) {
            expect = 0;
            continue;
        }
        if ((e[11] & 0x3F) == ATTR_LFN) {
            int ord = e[0] & 0x3F;
            if (e[0] & 0x40) {
                if (ord == 0 || ord > 20) {
                    expect = 0;
                    continue;
                }
                memset(lfn, 0xFF, sizeof(lfn));
                expect = ord;
                chk = e[13];
                lfn_first = idx;
            } else if (expect == 0 || ord != expect - 1 || e[13] != chk) {
                expect = 0;
                continue;
            } else {
                expect = ord;
            }
            for (uint32_t i = 0; i < LFN_CHARS; i++)
                lfn[(ord - 1) * LFN_CHARS + i] = rd16(e + k_lfn_offsets[i]);
            continue;
        }
        if (e[11] & ATTR_VOLUME) {
            expect = 0;
            continue;
        }
        if (expect == 1 && lfn_checksum(e) == chk) {
            size_t n = 0;
            while (n < sizeof(lfn) / sizeof(lfn[0]) && lfn[n] != 0 && lfn[n] != 0xFFFF)
                n++;
            if (name_eq_u16(lfn, n, name)) {
                hit->ent = e;
                hit->first = lfn_first;
                hit->last = idx;
                return true;
            }
        }
        expect = 0;
        if (name_eq_u16(disp, short_to_display(e, disp), name)) {
            hit->ent = e;
            hit->first = idx;
            hit->last = idx;
            return true;
        }
    }
}

static bool dir_short_exists(fat_vol_t *v, uint32_t dir, const uint8_t sn[11])
{
    for (uint32_t idx = 0;; idx++) {
        uint8_t *e = dir_slot(v, dir, idx, false);
        if (!e || e[0] == 0x00)
            return false;
        if (e[0] == 0xE5 || (e[11] & 0x3F) == ATTR_LFN)
            continue;
        if (memcmp(e, sn, 11) == 0)
            return true;
    }
}

static bool short_char_ok(unsigned c)
{
    return c > 0x20 && c < 0x7F && !strchr("\"*+,./:;<=>?[\\]|", (int)c);
}

/*
 * Build an 8.3 name for `name`. Returns true when a long-name entry set is
 * needed (the 8.3 form does not reproduce the name exactly).
 */
static bool make_short_name(fat_vol_t *v, uint32_t dir, const char *name,
                            uint8_t sn[11])
{
    const char *dot = strrchr(name, '.');
    char base[256], ext[4];
    size_t blen = 0, elen = 0;
    bool lossy = false;
    char exact[13];

    if (dot == name)
        dot = NULL;
    for (const char *p = name; *p && (!dot || p < dot); p++) {
        unsigned c = (unsigned char)*p;
        if (c == ' ' || c == '.') {
            lossy = true;
            continue;
        }
        if (!short_char_ok(c)) {
            c = '_';
            lossy = true;
        }
        if (blen < sizeof(base) - 1)
            base[blen++] = (char)upper_ascii((int)c);
    }
    base[blen] = '\0';
    if (dot) {
        for (const char *p = dot + 1; *p; p++) {
            unsigned c = (unsigned char)*p;
            if (c == ' ') {
                lossy = true;
                continue;
            }
            if (!short_char_ok(c)) {
                c = '_';
                lossy = true;
            }
            if (elen < 3)
                ext[elen++] = (char)upper_ascii((int)c);
            else
                lossy = true;
        }
    }
    ext[elen] = '\0';
    if (blen == 0) {
        memcpy(base, "_", 2);
        blen = 1;
        lossy = true;
    }
    if (blen > 8)
        lossy = true;

    memset(sn, ' ', 11);
    if (!lossy) {
        memcpy(sn, base, blen);
        memcpy(sn + 8, ext, elen);
        if (!dir_short_exists(v, dir, sn)) {
            snprintf(exact, sizeof(exact), "%s%s%s", base, elen ? "." : "", ext);
            if (sn[0] == 0xE5)
                sn[0] = 0x05;
            return strcmp(exact, name) != 0;
        }
    }
    for (unsigned n = 1; n < 1000000u; n++) {
        char tail[9];
        int tlen = snprintf(tail, sizeof(tail), "~%u", n);
        size_t keep = blen < (size_t)(8 - tlen) ? blen : (size_t)(8 - tlen);

        memset(sn, ' ', 11);
        memcpy(sn, base, keep);
        memcpy(sn + keep, tail, (size_t)tlen);
        memcpy(sn + 8, ext, elen);
        if (!dir_short_exists(v, dir, sn))
            break;
    }
    if (sn[0] == 0xE5)
        sn[0] = 0x05;
    return true;
}

static void fat_now(uint16_t *date, uint16_t *tm)
{
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);

    if (!lt || lt->tm_year < 80) {
        *date = (uint16_t)((1 << 5) | 1);
        *tm = 0;
        return;
    }
    *date = (uint16_t)(((lt->tm_year - 80) << 9) | ((lt->tm_mon + 1) << 5) | lt->tm_mday);
    *tm = (uint16_t)((lt->tm_hour << 11) | (lt->tm_min << 5) | (lt->tm_sec / 2));
}

static void dirent_set_time(uint8_t *e)
{
    uint16_t d, t;

    fat_now(&d, &t);
    wr16(e + 14, t);
    wr16(e + 16, d);
    wr16(e + 18, d);
    wr16(e + 22, t);
    wr16(e + 24, d);
}

/* Create a new entry set; returns the 8.3 entry or NULL (directory full). */
static uint8_t *dir_create(fat_vol_t *v, uint32_t dir, const char *name, uint8_t attr)
{
    uint8_t sn[11];
    size_t nlen = strlen(name);
    bool need_lfn = make_short_name(v, dir, name, sn);
    uint32_t nlfn = need_lfn ? (uint32_t)((nlen + LFN_CHARS - 1) / LFN_CHARS) : 0;
    uint32_t need = nlfn + 1;
    uint32_t run = 0, start = 0;
    uint8_t chk = lfn_checksum(sn);
    uint8_t *e;

    for (uint32_t idx = 0;; idx++) {
        e = dir_slot(v, dir, idx, true);
        if (!e)
            return NULL;
        if (e[0] == 0x00 || e[0] == 0xE5) {
            if (run == 0)
                start = idx;
            if (++run == need)
                break;
        } else {
            run = 0;
        }
    }

    for (uint32_t i = 0; i < nlfn; i++) {
        uint32_t ord = nlfn - i;
        e = dir_slot(v, dir, start + i, false);
        memset(e, 0, DIRENT_SIZE);
        e[0] = (uint8_t)(ord | (i == 0 ? 0x40 : 0));
        e[11] = ATTR_LFN;
        e[13] = chk;
        for (uint32_t k = 0; k < LFN_CHARS; k++) {
            size_t pos = (ord - 1) * LFN_CHARS + k;
            uint16_t ch = pos < nlen ? (uint16_t)(unsigned char)name[pos]
                        : pos == nlen ? 0x0000 : 0xFFFF;
            wr16(e + k_lfn_offsets[k], ch);
        }
    }
    e = dir_slot(v, dir, start + nlfn, false);
    memset(e, 0, DIRENT_SIZE);
    memcpy(e, sn, 11);
    e[11] = attr;
    dirent_set_time(e);
    return e;
}

static uint32_t dirent_cluster(const uint8_t *e) { return rd16(e + 26); }

/* Walk `path` (backslash separated) creating directories as needed. */
static bool fat_mkdirs(fat_vol_t *v, const char *path, uint32_t *out_dir,
                       const char **leaf, logbuf_t *log)
{
    char comp[MAX_NAME + 1];
    uint32_t dir = 0;
    const char *p = path;

    for (;;) {
        const char *sep = strchr(p, '\\');
        size_t n;
        dir_hit_t hit;

        if (!sep) {
            *leaf = p;
            *out_dir = dir;
            return *p != '\0';
        }
        n = (size_t)(sep - p);
        if (n == 0) {
            p = sep + 1;
            continue;
        }
        if (n > MAX_NAME) {
            log_add(log, "error: path component too long in %s\n", path);
            return false;
        }
        memcpy(comp, p, n);
        comp[n] = '\0';
        if (dir_find(v, dir, comp, &hit)) {
            if (!(hit.ent[11] & ATTR_DIR)) {
                log_add(log, "error: %s: '%s' exists and is not a directory\n", path, comp);
                return false;
            }
            dir = dirent_cluster(hit.ent);
        } else {
            uint32_t c = fat_alloc(v);
            uint8_t *e, *dot;
            if (!c) {
                log_add(log, "error: disk full creating %s\n", comp);
                return false;
            }
            e = dir_create(v, dir, comp, ATTR_DIR);
            if (!e) {
                fat_set(v, c, 0);
                log_add(log, "error: directory full creating %s\n", comp);
                return false;
            }
            wr16(e + 26, c);
            dot = fat_cluster_ptr(v, c);
            memset(dot, ' ', 11);
            dot[0] = '.';
            dot[11] = ATTR_DIR;
            dirent_set_time(dot);
            wr16(dot + 26, c);
            memset(dot + DIRENT_SIZE, ' ', 11);
            dot[DIRENT_SIZE] = '.';
            dot[DIRENT_SIZE + 1] = '.';
            dot[DIRENT_SIZE + 11] = ATTR_DIR;
            dirent_set_time(dot + DIRENT_SIZE);
            wr16(dot + DIRENT_SIZE + 26, dir);
            dir = c;
        }
        p = sep + 1;
    }
}

static bool fat_write_file(fat_vol_t *v, const char *path, const uint8_t *data,
                           size_t len, logbuf_t *log)
{
    uint32_t dir, first = 0, prev = 0;
    uint32_t nclusters = (uint32_t)((len + v->cluster_bytes - 1) / v->cluster_bytes);
    const char *leaf;
    dir_hit_t hit;
    uint8_t *e;

    if (len > 0xFFFFFFFFu) {
        log_add(log, "error: %s is too large\n", path);
        return false;
    }
    if (!fat_mkdirs(v, path, &dir, &leaf, log))
        return false;
    if (strlen(leaf) > MAX_NAME) {
        log_add(log, "error: file name too long: %s\n", leaf);
        return false;
    }
    if (dir_find(v, dir, leaf, &hit)) {
        if (hit.ent[11] & ATTR_DIR) {
            log_add(log, "error: %s exists and is a directory\n", path);
            return false;
        }
        fat_free_chain(v, dirent_cluster(hit.ent));
        e = hit.ent;
        wr16(e + 26, 0);
        wr32(e + 28, 0);
    } else {
        e = NULL;
    }
    if (fat_free_clusters(v) < nclusters + 1u) {
        log_add(log, "error: disk full writing %s (%zu bytes)\n", path, len);
        return false;
    }
    if (!e) {
        e = dir_create(v, dir, leaf, 0x20);
        if (!e) {
            log_add(log, "error: directory full writing %s\n", path);
            return false;
        }
    }
    for (uint32_t i = 0; i < nclusters; i++) {
        uint32_t c = fat_alloc(v);
        size_t off = (size_t)i * v->cluster_bytes;
        size_t chunk = len - off < v->cluster_bytes ? len - off : v->cluster_bytes;
        if (!c) {
            log_add(log, "error: disk full writing %s\n", path);
            return false;
        }
        memcpy(fat_cluster_ptr(v, c), data + off, chunk);
        if (prev)
            fat_set(v, prev, c);
        else
            first = c;
        prev = c;
    }
    wr16(e + 26, first);
    wr32(e + 28, (uint32_t)len);
    dirent_set_time(e);
    return true;
}

static uint8_t *fat_read(fat_vol_t *v, const char *path, size_t *out_len)
{
    char comp[MAX_NAME + 1];
    uint32_t dir = 0;
    const char *p = path;
    dir_hit_t hit;

    for (;;) {
        const char *sep = strchr(p, '\\');
        size_t n = sep ? (size_t)(sep - p) : strlen(p);
        if (n == 0 || n > MAX_NAME)
            return NULL;
        memcpy(comp, p, n);
        comp[n] = '\0';
        if (!dir_find(v, dir, comp, &hit))
            return NULL;
        if (!sep)
            break;
        if (!(hit.ent[11] & ATTR_DIR))
            return NULL;
        dir = dirent_cluster(hit.ent);
        p = sep + 1;
    }
    if (hit.ent[11] & ATTR_DIR)
        return NULL;

    {
        uint32_t size = rd32(hit.ent + 28);
        uint32_t c = dirent_cluster(hit.ent);
        uint8_t *buf = malloc(size ? size : 1);
        size_t got = 0;
        uint32_t guard = v->cluster_count;

        if (!buf)
            return NULL;
        while (got < size) {
            size_t chunk;
            if (!fat_valid_cluster(v, c) || !guard--) {
                free(buf);
                return NULL;
            }
            chunk = size - got < v->cluster_bytes ? size - got : v->cluster_bytes;
            memcpy(buf + got, fat_cluster_ptr(v, c), chunk);
            got += chunk;
            c = fat_get(v, c);
        }
        *out_len = size;
        return buf;
    }
}

uint8_t *cbea_fat_read_file(const uint8_t *image, size_t image_len,
                            const char *path, size_t *out_len)
{
    fat_vol_t v;

    /* fat_read never writes; the cast only satisfies the shared struct. */
    if (!fat_locate((uint8_t *)image, image_len, &v))
        return NULL;
    return fat_read(&v, path, out_len);
}

int cbea_fat_write_file(uint8_t *image, size_t image_len, const char *path,
                        const uint8_t *data, size_t len,
                        char *log, size_t log_len)
{
    logbuf_t l = { log, log_len, 0 };
    fat_vol_t v;

    if (log && log_len)
        log[0] = '\0';
    if (!fat_locate(image, image_len, &v)) {
        log_add(&l, "error: no FAT16 volume in image\n");
        return -1;
    }
    return fat_write_file(&v, path, data, len, &l) ? 0 : -1;
}

/*
 * Bare FAT16 "superfloppy" as tools/make_cf_image.sh builds with mformat:
 * BPB at sector 0, no MBR, 512 root entries, 16 heads x 32 sectors/track.
 * The cluster size is the smallest (from 4 sectors at 16 MB and up) that
 * keeps the cluster count within the FAT16 range (4085..65524).
 */
int cbea_fat_format(uint8_t *image, size_t image_len, const char *label)
{
    const uint32_t reserved = 1, nfats = 2, root_entries = 512;
    const uint32_t root_sectors = root_entries * DIRENT_SIZE / SECTOR_SIZE;
    uint32_t total, spc, fat_sectors = 0, clusters = 0;
    uint8_t name[11];
    uint8_t *b = image, *e;
    uint16_t d, t;

    if (!image || image_len % SECTOR_SIZE != 0 ||
        image_len < 8u * 1024u * 1024u || image_len > 1024u * 1024u * 1024u)
        return -1;
    total = (uint32_t)(image_len / SECTOR_SIZE);
    /* 4 sectors/cluster from 16 MB up matches the script's mformat -c 4. */
    for (spc = total >= 32768u ? 4 : 2; spc <= 64; spc *= 2) {
        uint32_t avail = total - reserved - root_sectors;
        fat_sectors = (avail + 256 * spc + nfats - 1) / (256 * spc + nfats);
        clusters = (avail - nfats * fat_sectors) / spc;
        if (clusters <= 65524)
            break;
    }
    if (spc > 64 || clusters < 4085)
        return -1;

    memset(name, ' ', sizeof(name));
    for (size_t i = 0; label && label[i] && i < sizeof(name); i++)
        name[i] = (uint8_t)upper_ascii((unsigned char)label[i]);

    memset(image, 0, (size_t)(reserved + nfats * fat_sectors + root_sectors) * SECTOR_SIZE);
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
    memcpy(b + 3, "BE300EMU", 8);
    wr16(b + 0x0B, SECTOR_SIZE);
    b[0x0D] = (uint8_t)spc;
    wr16(b + 0x0E, reserved);
    b[0x10] = (uint8_t)nfats;
    wr16(b + 0x11, root_entries);
    if (total < 0x10000u)
        wr16(b + 0x13, total);
    else
        wr32(b + 0x20, total);
    b[0x15] = 0xF8;
    wr16(b + 0x16, fat_sectors);
    wr16(b + 0x18, 32);
    wr16(b + 0x1A, 16);
    b[0x24] = 0x80;
    b[0x26] = 0x29;
    fat_now(&d, &t);
    wr32(b + 0x27, ((uint32_t)d << 16) | t);
    memcpy(b + 0x2B, name, 11);
    memcpy(b + 0x36, "FAT16   ", 8);
    b[0x1FE] = 0x55; b[0x1FF] = 0xAA;

    for (uint32_t i = 0; i < nfats; i++) {
        uint8_t *fat = b + (reserved + i * fat_sectors) * SECTOR_SIZE;
        wr16(fat, 0xFFF8);
        wr16(fat + 2, 0xFFFF);
    }
    e = b + (reserved + nfats * fat_sectors) * SECTOR_SIZE;
    memcpy(e, name, 11);
    e[11] = ATTR_VOLUME;
    wr16(e + 22, t);
    wr16(e + 24, d);
    return 0;
}

/* ------------------------------------------------------------------ */
/* .cbea archive                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[MAX_NAME + 1];
    const uint8_t *data;
    uint32_t size;
} cbea_entry_t;

typedef struct {
    cbea_entry_t *entries;
    uint32_t count;
} cbea_t;

static bool cbea_parse(const uint8_t *pkg, size_t len, cbea_t *out, logbuf_t *log)
{
    uint32_t count;
    size_t p = 0x100;

    memset(out, 0, sizeof(*out));
    if (len < 0x100) {
        log_add(log, "error: package too short to be a .cbea file\n");
        return false;
    }
    count = rd32(pkg + 2);
    if (count == 0 || count > 4096) {
        log_add(log, "error: bad .cbea file count %u\n", count);
        return false;
    }
    out->entries = calloc(count, sizeof(*out->entries));
    if (!out->entries) {
        log_add(log, "error: out of memory\n");
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        cbea_entry_t *e = &out->entries[i];
        uint32_t nlen, size;

        if (len - p < 8)
            goto truncated;
        nlen = pkg[p + 7];
        p += 8;
        if (len - p < (size_t)nlen + 1 + 4)
            goto truncated;
        memcpy(e->name, pkg + p, nlen);
        e->name[nlen] = '\0';
        p += nlen + 1u;
        size = rd32(pkg + p);
        p += 4;
        if (len - p < size)
            goto truncated;
        e->data = pkg + p;
        e->size = size;
        p += size;
    }
    out->count = count;
    return true;

truncated:
    log_add(log, "error: .cbea archive is truncated\n");
    free(out->entries);
    out->entries = NULL;
    return false;
}

static const cbea_entry_t *cbea_find(const cbea_t *c, const char *name)
{
    for (uint32_t i = 0; i < c->count; i++) {
        if (str_ieq(c->entries[i].name, name))
            return &c->entries[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Install.inf                                                        */
/* ------------------------------------------------------------------ */

#define INF_MAX_FIELDS 64

typedef struct {
    char *s[INF_MAX_FIELDS];
    bool quoted[INF_MAX_FIELDS];
    int n;
} inf_fields_t;

/* Split a line into comma-separated fields; modifies `line` in place. */
static void inf_split(char *line, inf_fields_t *f)
{
    char *p = line;

    f->n = 0;
    while (f->n < INF_MAX_FIELDS) {
        char *start, *end;

        while (*p == ' ' || *p == '\t')
            p++;
        f->quoted[f->n] = false;
        if (*p == '"') {
            start = ++p;
            while (*p && *p != '"')
                p++;
            end = p;
            if (*p == '"')
                p++;
            f->quoted[f->n] = true;
            while (*p && *p != ',')
                p++;
        } else {
            start = p;
            while (*p && *p != ',')
                p++;
            end = p;
            while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
                end--;
        }
        f->s[f->n++] = start;
        if (*p == ',') {
            *end = '\0';
            p++;
            continue;
        }
        *end = '\0';
        break;
    }
}

static char *str_trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        e--;
    *e = '\0';
    return s;
}

static char *str_unquote(char *s)
{
    size_t n = strlen(s);

    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        s[n - 1] = '\0';
        return s + 1;
    }
    return s;
}

/* Replace every case-insensitive occurrence of `token` with `with`. */
static bool str_subst(char *s, size_t cap, const char *token, const char *with)
{
    size_t tlen = strlen(token), wlen = strlen(with);
    char *p = s;

    while (*p) {
        if (str_iprefix(p, token)) {
            size_t rest = strlen(p + tlen);
            if ((size_t)(p - s) + wlen + rest + 1 > cap)
                return false;
            memmove(p + wlen, p + tlen, rest + 1);
            memcpy(p, with, wlen);
            p += wlen;
        } else {
            p++;
        }
    }
    return true;
}

typedef struct {
    char install_dir[MAX_PATH_LEN];
    char program[256];
    char caption[256];
    char exe_file[MAX_PATH_LEN];
    char uninstall[256];
    char version[32];
    int unload;
    logbuf_t *log;
} inf_ctx_t;

/*
 * Map an installer destination to a path inside \Nand Disk.
 * ?Drive? is the install volume; this tool always installs to main memory
 * (\Nand Disk). \Windows is the volatile object store, so single files
 * aimed there go to \Nand Disk\Program Files\Patch, which the BE-300 shadows
 * into \Windows on every cold boot.
 */
static bool resolve_dest(inf_ctx_t *ctx, const char *in, char *out, size_t cap)
{
    char buf[MAX_PATH_LEN];
    char norm[MAX_PATH_LEN];
    size_t n = 0;
    const char *rel;

    if (strlen(in) >= sizeof(buf))
        return false;
    strcpy(buf, in);
    if (!str_subst(buf, sizeof(buf), "%INSTALL_DIR%", ctx->install_dir) ||
        !str_subst(buf, sizeof(buf), "?Drive?", "\\Nand Disk"))
        return false;
    for (const char *p = buf; *p && n + 1 < sizeof(norm); p++) {
        char c = *p == '/' ? '\\' : *p;
        if (c == '\\' && n > 0 && norm[n - 1] == '\\')
            continue;
        norm[n++] = c;
    }
    norm[n] = '\0';
    while (n > 0 && norm[n - 1] == '\\')
        norm[--n] = '\0';

    if (str_iprefix(norm, "\\Nand Disk\\")) {
        rel = norm + strlen("\\Nand Disk\\");
        if (strlen(rel) + 1 > cap)
            return false;
        strcpy(out, rel);
        return *out != '\0';
    }
    if (str_iprefix(norm, "\\Windows\\") &&
        !strchr(norm + strlen("\\Windows\\"), '\\')) {
        int w = snprintf(out, cap, "Program Files\\Patch\\%s",
            norm + strlen("\\Windows\\"));
        if (w < 0 || (size_t)w >= cap)
            return false;
        log_add(ctx->log, "  note: %s -> \\Nand Disk\\%s (shadowed into \\Windows at boot)\n",
            norm, out);
        return true;
    }
    log_add(ctx->log, "error: unsupported install destination %s (only \\Nand Disk and \\Windows files)\n",
        norm);
    return false;
}

/* ------------------------------------------------------------------ */
/* compact registry (\Backup\System.reg)                              */
/* ------------------------------------------------------------------ */

/*
 * Record layout (decoded from System.reg on nand/300.bin and nand/expod.bin,
 * matching tools/inject_stowaway.py parse_reg_records):
 *   header: magic b2 74 83 1d, u32 total size
 *   key:    u16 payload_len, u16 1, u16 root, u16 nchars, u16 class_nchars,
 *           UTF-16Z path, UTF-16Z class (absent when class_nchars == 0)
 *   value:  u16 payload_len, u16 2, u16 type, u16 nchars, u16 data_len,
 *           UTF-16Z name, data
 * (inject_stowaway.py reads nchars as a u32, which only holds while the
 * class is empty; a guest-saved snapshot has HKLM\SOFTWARE\Microsoft\Clock
 * with class "Preon".)
 * Keys are unique and flat (no parent records needed); roots are
 * 0 = HKCR, 1 = HKCU, 2 = HKLM. Default values use the literal name
 * "Default" (496 such records in expod.bin, e.g. HKCR\.tmv).
 */

typedef struct {
    uint32_t root;
    char key[MAX_PATH_LEN];
    char name[256];
    uint32_t type;
    uint8_t *data;
    size_t len;
    bool done;
} reg_add_t;

typedef struct {
    reg_add_t *items;
    size_t count;
    size_t cap;
} reg_list_t;

static void reg_list_free(reg_list_t *l)
{
    for (size_t i = 0; i < l->count; i++)
        free(l->items[i].data);
    free(l->items);
    memset(l, 0, sizeof(*l));
}

static size_t utf16z_len(const char *s) { return (strlen(s) + 1) * 2; }

static void put_utf16z(uint8_t *out, const char *s)
{
    size_t i = 0;

    for (; s[i]; i++)
        wr16(out + i * 2, (unsigned char)s[i]);
    wr16(out + i * 2, 0);
}

/* Compare a UTF-16 registry string (nchars incl NUL) with an ANSI one. */
static bool reg_name_eq(const uint8_t *u, uint32_t nchars, const char *s)
{
    uint16_t tmp[MAX_PATH_LEN];
    uint32_t n = 0;

    while (n < nchars && n < MAX_PATH_LEN) {
        tmp[n] = rd16(u + n * 2);
        if (tmp[n] == 0)
            break;
        n++;
    }
    return name_eq_u16(tmp, n, s);
}

static bool buf_append(uint8_t **buf, size_t *len, size_t *cap,
                       const void *src, size_t n)
{
    if (*len + n > *cap) {
        size_t nc = *cap ? *cap * 2 : 4096;
        uint8_t *nb;
        while (nc < *len + n)
            nc *= 2;
        nb = realloc(*buf, nc);
        if (!nb)
            return false;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, src, n);
    *len += n;
    return true;
}

static bool reg_emit_key(uint8_t **o, size_t *ol, size_t *oc, uint32_t root, const char *key)
{
    uint8_t rec[10 + MAX_PATH_LEN * 2];
    size_t kb = utf16z_len(key);

    wr16(rec, (uint32_t)(6 + kb));
    wr16(rec + 2, 1);
    wr16(rec + 4, root);
    wr16(rec + 6, (uint32_t)(kb / 2));
    wr16(rec + 8, 0);
    put_utf16z(rec + 10, key);
    return buf_append(o, ol, oc, rec, 10 + kb);
}

static bool reg_emit_value(uint8_t **o, size_t *ol, size_t *oc, const reg_add_t *a)
{
    uint8_t hdr[10 + 512];
    size_t nb = utf16z_len(a->name);

    wr16(hdr, (uint32_t)(6 + nb + a->len));
    wr16(hdr + 2, 2);
    wr16(hdr + 4, a->type);
    wr16(hdr + 6, (uint32_t)(nb / 2));
    wr16(hdr + 8, (uint32_t)a->len);
    put_utf16z(hdr + 10, a->name);
    return buf_append(o, ol, oc, hdr, 10 + nb) &&
           buf_append(o, ol, oc, a->data, a->len);
}

/* Emit the pending values for (root, key), marking them done. */
static bool reg_emit_pending(uint8_t **o, size_t *ol, size_t *oc,
                             reg_list_t *adds, uint32_t root, const char *key)
{
    for (size_t i = 0; i < adds->count; i++) {
        reg_add_t *a = &adds->items[i];
        bool superseded = false;

        if (a->done || a->root != root || !str_ieq(a->key, key))
            continue;
        /* Later lines for the same value win, like a sequential RegSetValue. */
        for (size_t j = i + 1; j < adds->count; j++) {
            if (adds->items[j].root == root && str_ieq(adds->items[j].key, key) &&
                str_ieq(adds->items[j].name, a->name))
                superseded = true;
        }
        a->done = true;
        if (!superseded && !reg_emit_value(o, ol, oc, a))
            return false;
    }
    return true;
}

static bool reg_key_pending(const reg_list_t *adds, uint32_t root, const uint8_t *u,
                            uint32_t nchars, const char **key_out)
{
    for (size_t i = 0; i < adds->count; i++) {
        if (adds->items[i].root == root &&
            reg_name_eq(u, nchars, adds->items[i].key)) {
            *key_out = adds->items[i].key;
            return true;
        }
    }
    return false;
}

static bool reg_value_overridden(const reg_list_t *adds, uint32_t root, const char *key,
                                 const uint8_t *u, uint32_t nchars)
{
    for (size_t i = 0; i < adds->count; i++) {
        const reg_add_t *a = &adds->items[i];
        if (a->root == root && str_ieq(a->key, key) && reg_name_eq(u, nchars, a->name))
            return true;
    }
    return false;
}

/* Merge `adds` into an existing snapshot (or a new one when old == NULL). */
static uint8_t *reg_merge(const uint8_t *old, size_t old_len, reg_list_t *adds,
                          size_t *out_len, logbuf_t *log)
{
    uint8_t *o = NULL;
    size_t ol = 0, oc = 0;
    size_t pos = 8;
    const char *cur_key = NULL;
    uint32_t cur_root = 0;
    uint8_t hdr[8];

    if (old) {
        if (old_len < 8 || memcmp(old, k_reg_magic, 4) != 0 || rd32(old + 4) != old_len) {
            log_add(log, "error: \\Backup\\System.reg has an unknown format\n");
            return NULL;
        }
    }
    memcpy(hdr, k_reg_magic, 4);
    wr32(hdr + 4, 0);
    if (!buf_append(&o, &ol, &oc, hdr, 8))
        goto oom;

    while (old && pos < old_len) {
        uint32_t plen, rtype;
        const uint8_t *r = old + pos;

        if (old_len - pos < 4)
            goto corrupt;
        plen = rd16(r);
        rtype = rd16(r + 2);
        if (plen == 0 || old_len - pos < 4u + plen)
            goto corrupt;
        if (rtype == 1) {
            uint32_t nchars;
            if (plen < 6)
                goto corrupt;
            /* Flush pending values for the key that just ended. */
            if (cur_key && !reg_emit_pending(&o, &ol, &oc, adds, cur_root, cur_key))
                goto oom;
            cur_root = rd16(r + 4);
            nchars = rd16(r + 6);
            if (((uint32_t)nchars + rd16(r + 8)) * 2u > plen - 6)
                goto corrupt;
            if (!reg_key_pending(adds, cur_root, r + 10, nchars, &cur_key))
                cur_key = NULL;
            if (!buf_append(&o, &ol, &oc, r, 4u + plen))
                goto oom;
        } else if (rtype == 2) {
            uint32_t nchars;
            if (plen < 6)
                goto corrupt;
            nchars = rd16(r + 6);
            if ((uint64_t)nchars * 2 > plen - 6)
                goto corrupt;
            if (!(cur_key && reg_value_overridden(adds, cur_root, cur_key, r + 10, nchars))) {
                if (!buf_append(&o, &ol, &oc, r, 4u + plen))
                    goto oom;
            }
        } else {
            goto corrupt;
        }
        pos += 4u + plen;
    }
    if (cur_key && !reg_emit_pending(&o, &ol, &oc, adds, cur_root, cur_key))
        goto oom;

    /* Remaining keys are new: append each once with its values. */
    for (size_t i = 0; i < adds->count; i++) {
        reg_add_t *a = &adds->items[i];
        if (a->done)
            continue;
        if (!reg_emit_key(&o, &ol, &oc, a->root, a->key) ||
            !reg_emit_pending(&o, &ol, &oc, adds, a->root, a->key))
            goto oom;
    }
    wr32(o + 4, (uint32_t)ol);
    *out_len = ol;
    return o;

corrupt:
    log_add(log, "error: \\Backup\\System.reg is corrupt at offset 0x%zx\n", pos);
    free(o);
    return NULL;
oom:
    log_add(log, "error: out of memory\n");
    free(o);
    return NULL;
}

static bool parse_u32(const char *s, int base, uint32_t *out)
{
    char *end;
    unsigned long v;

    while (*s == ' ')
        s++;
    if (*s == '\0')
        return false;
    v = strtoul(s, &end, base);
    while (*end == ' ')
        end++;
    if (*end != '\0' || v > 0xFFFFFFFFul)
        return false;
    *out = (uint32_t)v;
    return true;
}

/* Append `a` (taking ownership of a->data). */
static bool reg_push(reg_list_t *adds, reg_add_t *a)
{
    if (adds->count == adds->cap) {
        size_t nc = adds->cap ? adds->cap * 2 : 32;
        reg_add_t *ni = realloc(adds->items, nc * sizeof(*ni));
        if (!ni) {
            free(a->data);
            return false;
        }
        adds->items = ni;
        adds->cap = nc;
    }
    adds->items[adds->count++] = *a;
    return true;
}

static bool reg_push_value(reg_list_t *adds, uint32_t root, const char *key,
                           const char *name, uint32_t type, const char *sz,
                           uint32_t dword)
{
    reg_add_t a;

    memset(&a, 0, sizeof(a));
    if (strlen(key) >= sizeof(a.key) || strlen(name) >= sizeof(a.name))
        return false;
    a.root = root;
    strcpy(a.key, key);
    strcpy(a.name, name);
    a.type = type;
    a.len = type == REG_TYPE_DWORD ? 4 : utf16z_len(sz);
    a.data = malloc(a.len);
    if (!a.data)
        return false;
    if (type == REG_TYPE_DWORD)
        wr32(a.data, dword);
    else
        put_utf16z(a.data, sz);
    return reg_push(adds, &a);
}

/*
 * Launcher entry. The Casio menu (COShell MainMenu = menu.exe) lists
 * HKLM\SOFTWARE\MENU\<app>. Entries written by the device installer for
 * Tweak, RegEdit, Notepad, GSFinderPPC, BeIe and Tapright in
 * nand/beshell.bin's System.reg all have this shape:
 *   ANAME = Caption, APATH = ExeFile, ACMND = "", ULOADFILE = Uninstall,
 *   ULOAD = Unload, APPNO = 8, SETTING_DLL/OP_DATA/SOUNDBMP/WAVE/ICON = "",
 *   INSTALL_DIR = resolved InstallDir, and \VERSION MAJOR/MINOR.
 * The key is named after the package's Program value.
 */
static bool reg_add_menu_entry(inf_ctx_t *ctx, reg_list_t *adds)
{
    static const char *const k_empty[] = {
        "SETTING_DLL", "OP_DATA", "SOUNDBMP", "WAVE", "ICON",
    };
    char key[MAX_PATH_LEN], vkey[MAX_PATH_LEN];
    const char *name = ctx->program[0] ? ctx->program : ctx->caption;
    unsigned major = 0, minor = 0;
    bool ok;

    if (!ctx->exe_file[0] || !name[0])
        return true;
    if (snprintf(key, sizeof(key), "SOFTWARE\\MENU\\%s", name) >= (int)sizeof(key) ||
        snprintf(vkey, sizeof(vkey), "%s\\VERSION", key) >= (int)sizeof(vkey))
        return false;
    sscanf(ctx->version, "%u.%u", &major, &minor);

    ok = reg_push_value(adds, REG_ROOT_HKLM, key, "APPNO", REG_TYPE_DWORD, NULL, 8) &&
         reg_push_value(adds, REG_ROOT_HKLM, key, "ULOAD", REG_TYPE_DWORD, NULL,
             ctx->unload > 0 ? 1u : 0u) &&
         reg_push_value(adds, REG_ROOT_HKLM, key, "ULOADFILE", REG_TYPE_SZ, ctx->uninstall, 0) &&
         reg_push_value(adds, REG_ROOT_HKLM, key, "ACMND", REG_TYPE_SZ, "", 0) &&
         reg_push_value(adds, REG_ROOT_HKLM, key, "APATH", REG_TYPE_SZ, ctx->exe_file, 0) &&
         reg_push_value(adds, REG_ROOT_HKLM, key, "ANAME", REG_TYPE_SZ,
             ctx->caption[0] ? ctx->caption : name, 0);
    for (size_t i = 0; ok && i < sizeof(k_empty) / sizeof(k_empty[0]); i++)
        ok = reg_push_value(adds, REG_ROOT_HKLM, key, k_empty[i], REG_TYPE_SZ, "", 0);
    ok = ok &&
         reg_push_value(adds, REG_ROOT_HKLM, key, "INSTALL_DIR", REG_TYPE_SZ, ctx->install_dir, 0) &&
         reg_push_value(adds, REG_ROOT_HKLM, vkey, "MINOR", REG_TYPE_DWORD, NULL, minor) &&
         reg_push_value(adds, REG_ROOT_HKLM, vkey, "MAJOR", REG_TYPE_DWORD, NULL, major);
    if (ok)
        log_add(ctx->log, "  menu: %s -> %s\n", ctx->caption[0] ? ctx->caption : name,
            ctx->exe_file);
    return ok;
}

static bool reg_parse_line(inf_ctx_t *ctx, inf_fields_t *f, reg_list_t *adds)
{
    reg_add_t a;
    const char *root = f->s[0];
    const char *type;
    size_t vlen;

    memset(&a, 0, sizeof(a));
    if (f->n < 5) {
        log_add(ctx->log, "error: short [Add Registry] line for %s\n", f->n > 1 ? f->s[1] : root);
        return false;
    }
    if (str_ieq(root, "HKEY_LOCAL_MACHINE") || str_ieq(root, "HKLM"))
        a.root = REG_ROOT_HKLM;
    else if (str_ieq(root, "HKEY_CURRENT_USER") || str_ieq(root, "HKCU"))
        a.root = REG_ROOT_HKCU;
    else if (str_ieq(root, "HKEY_CLASSES_ROOT") || str_ieq(root, "HKCR"))
        a.root = REG_ROOT_HKCR;
    else {
        log_add(ctx->log, "error: unsupported registry root %s\n", root);
        return false;
    }
    if (strlen(f->s[1]) >= sizeof(a.key) || strlen(f->s[2]) >= sizeof(a.name) ||
        f->s[1][0] == '\0') {
        log_add(ctx->log, "error: bad registry key or value name\n");
        return false;
    }
    strcpy(a.key, f->s[1]);
    strcpy(a.name, f->s[2][0] ? f->s[2] : "Default");
    type = f->s[3];

    if (str_ieq(type, "REG_SZ") || str_ieq(type, "REG_EXPAND_SZ")) {
        char val[MAX_PATH_LEN];
        if (f->n < 6 || strlen(f->s[5]) >= sizeof(val)) {
            log_add(ctx->log, "error: bad REG_SZ value for %s\n", a.key);
            return false;
        }
        strcpy(val, f->s[5]);
        /*
         * INF strings cannot end in a bare backslash, so packages write
         * "...\\". The installed WMP 8 SkinDir in nand/expod.bin's
         * System.reg ends in a single '\', so collapse the escape.
         */
        vlen = strlen(val);
        if (vlen >= 2 && val[vlen - 1] == '\\' && val[vlen - 2] == '\\')
            val[vlen - 1] = '\0';
        a.type = str_ieq(type, "REG_SZ") ? REG_TYPE_SZ : REG_TYPE_EXPAND_SZ;
        a.len = utf16z_len(val);
        a.data = malloc(a.len);
        if (!a.data)
            return false;
        put_utf16z(a.data, val);
    } else if (str_ieq(type, "REG_DWORD")) {
        uint32_t v;
        /*
         * Casio's installer reads DWORD digits as hex: WMP's Install.inf
         * lists Buttons Action/Escape/Left = 13/27/37 and the installed
         * WMP 8 registry in nand/expod.bin holds 0x13/0x27/0x37.
         */
        if (f->n < 6 || !parse_u32(f->s[5], 16, &v)) {
            log_add(ctx->log, "error: bad REG_DWORD value for %s\\%s\n", a.key, a.name);
            return false;
        }
        a.type = REG_TYPE_DWORD;
        a.len = 4;
        a.data = malloc(4);
        if (!a.data)
            return false;
        wr32(a.data, v);
    } else if (str_ieq(type, "REG_BINARY")) {
        a.type = REG_TYPE_BINARY;
        a.data = malloc((size_t)(f->n > 5 ? f->n - 5 : 1));
        if (!a.data)
            return false;
        for (int i = 5; i < f->n; i++) {
            uint32_t b;
            if (!parse_u32(f->s[i], 16, &b) || b > 0xFF) {
                free(a.data);
                log_add(ctx->log, "error: bad REG_BINARY byte for %s\\%s\n", a.key, a.name);
                return false;
            }
            a.data[a.len++] = (uint8_t)b;
        }
    } else if (str_ieq(type, "REG_MULTI_SZ")) {
        size_t total = 2;
        a.type = REG_TYPE_MULTI_SZ;
        for (int i = 5; i < f->n; i++)
            total += utf16z_len(f->s[i]);
        a.data = malloc(total);
        if (!a.data)
            return false;
        for (int i = 5; i < f->n; i++) {
            put_utf16z(a.data + a.len, f->s[i]);
            a.len += utf16z_len(f->s[i]);
        }
        wr16(a.data + a.len, 0);
        a.len += 2;
    } else {
        log_add(ctx->log, "error: unsupported registry type %s for %s\n", type, a.key);
        return false;
    }
    if (6 + utf16z_len(a.name) + a.len > 0xFFFF || 6 + utf16z_len(a.key) > 0xFFFF) {
        free(a.data);
        log_add(ctx->log, "error: registry value too large: %s\\%s\n", a.key, a.name);
        return false;
    }

    return reg_push(adds, &a);
}

static const char *reg_root_name(uint32_t root)
{
    return root == REG_ROOT_HKLM ? "HKLM" : root == REG_ROOT_HKCU ? "HKCU" : "HKCR";
}

/* ------------------------------------------------------------------ */
/* install driver                                                     */
/* ------------------------------------------------------------------ */

typedef enum { SEC_NONE, SEC_APP, SEC_COPY, SEC_REG, SEC_OTHER } inf_section_t;

static int install_into(fat_vol_t *v, const cbea_t *pkg, logbuf_t *log)
{
    const cbea_entry_t *inf = cbea_find(pkg, "Install.inf");
    inf_ctx_t ctx;
    reg_list_t adds;
    char *text, *line, *next;
    inf_section_t sec = SEC_NONE;
    unsigned files = 0;
    int rc = -1;

    memset(&ctx, 0, sizeof(ctx));
    memset(&adds, 0, sizeof(adds));
    ctx.log = log;
    if (!inf) {
        log_add(log, "error: package has no Install.inf\n");
        return -1;
    }
    text = malloc((size_t)inf->size + 1);
    if (!text) {
        log_add(log, "error: out of memory\n");
        return -1;
    }
    memcpy(text, inf->data, inf->size);
    text[inf->size] = '\0';

    /* Pass 1: [App Information], so %INSTALL_DIR% is known for any order. */
    for (line = text; line; line = next) {
        char *s, *eq;
        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        s = str_trim(line);
        if (*s == '[') {
            sec = str_iprefix(s, "[App Information]") ? SEC_APP : SEC_OTHER;
            continue;
        }
        if (sec != SEC_APP || *s == ';' || !(eq = strchr(s, '=')))
            continue;
        *eq = '\0';
        {
            char *k = str_trim(s);
            char *val = str_unquote(str_trim(eq + 1));
            if (str_ieq(k, "InstallDir") && strlen(val) < sizeof(ctx.install_dir)) {
                strcpy(ctx.install_dir, val);
            } else if (str_ieq(k, "Program") && strlen(val) < sizeof(ctx.program)) {
                strcpy(ctx.program, val);
            } else if (str_ieq(k, "Caption") && strlen(val) < sizeof(ctx.caption)) {
                strcpy(ctx.caption, val);
            } else if (str_ieq(k, "ExeFile") && strlen(val) < sizeof(ctx.exe_file)) {
                strcpy(ctx.exe_file, val);
            } else if (str_ieq(k, "Uninstall") && strlen(val) < sizeof(ctx.uninstall)) {
                strcpy(ctx.uninstall, val);
            } else if (str_ieq(k, "Version") && strlen(val) < sizeof(ctx.version)) {
                strcpy(ctx.version, val);
            } else if (str_ieq(k, "Unload")) {
                ctx.unload = atoi(val);
            } else if (str_ieq(k, "Destination") && str_ieq(val, "STORAGE_ONLY")) {
                log_add(log, "error: package is STORAGE_ONLY (Storage Card install); "
                             "use the package's MAIN variant instead\n");
                goto out;
            }
        }
    }
    if (!str_subst(ctx.install_dir, sizeof(ctx.install_dir), "?Drive?", "\\Nand Disk"))
        goto out;
    log_add(log, "Installing %s\n", ctx.program[0] ? ctx.program : "(unnamed package)");

    /* Pass 2: files and registry. */
    memcpy(text, inf->data, inf->size);
    text[inf->size] = '\0';
    sec = SEC_NONE;
    for (line = text; line; line = next) {
        char *s;
        inf_fields_t f;

        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        s = str_trim(line);
        if (*s == '\0' || *s == ';')
            continue;
        if (*s == '[') {
            if (str_iprefix(s, "[App Information]"))
                sec = SEC_APP;
            else if (str_iprefix(s, "[Copy Files]"))
                sec = SEC_COPY;
            else if (str_iprefix(s, "[Add Registry]"))
                sec = SEC_REG;
            else
                sec = SEC_OTHER;
            continue;
        }
        if (sec == SEC_COPY) {
            char dest[MAX_PATH_LEN];
            const cbea_entry_t *src;
            dir_hit_t hit;
            uint32_t dir;
            const char *leaf;
            bool exists;

            inf_split(s, &f);
            if (f.n < 2) {
                log_add(log, "error: bad [Copy Files] line: %s\n", s);
                goto out;
            }
            src = cbea_find(pkg, f.s[1]);
            if (!src) {
                log_add(log, "error: package is missing %s\n", f.s[1]);
                goto out;
            }
            if (!resolve_dest(&ctx, f.s[0], dest, sizeof(dest))) {
                log_add(log, "error: cannot place %s\n", f.s[0]);
                goto out;
            }
            exists = false;
            if (f.n > 2 && strstr(f.s[2], "COPYFLG_NOOVERWRITE")) {
                if (fat_mkdirs(v, dest, &dir, &leaf, log) && dir_find(v, dir, leaf, &hit))
                    exists = true;
            }
            if (exists) {
                log_add(log, "  keep: \\Nand Disk\\%s (COPYFLG_NOOVERWRITE)\n", dest);
                continue;
            }
            if (!fat_write_file(v, dest, src->data, src->size, log))
                goto out;
            log_add(log, "  file: \\Nand Disk\\%s (%u bytes)\n", dest, src->size);
            files++;
        } else if (sec == SEC_REG) {
            inf_split(s, &f);
            if (!reg_parse_line(&ctx, &f, &adds))
                goto out;
        }
    }

    if (!reg_add_menu_entry(&ctx, &adds)) {
        log_add(log, "error: cannot build the launcher entry\n");
        goto out;
    }
    if (adds.count > 0) {
        size_t old_len = 0, new_len = 0;
        uint8_t *old = fat_read(v, "Backup\\System.reg", &old_len);
        uint8_t *merged = reg_merge(old, old_len, &adds, &new_len, log);
        bool ok;

        log_add(log, "  registry: %s \\Backup\\System.reg\n", old ? "updating" : "creating");
        free(old);
        if (!merged)
            goto out;
        ok = fat_write_file(v, "Backup\\System.reg", merged, new_len, log) &&
             fat_write_file(v, "Backup\\System.$$$", merged, new_len, log);
        free(merged);
        if (!ok)
            goto out;
        for (size_t i = 0; i < adds.count; i++)
            log_add(log, "  reg: %s\\%s [%s]\n", reg_root_name(adds.items[i].root),
                adds.items[i].key, adds.items[i].name);
    }
    log_add(log, "Installed %u files, %zu registry values\n", files, adds.count);
    rc = 0;

out:
    reg_list_free(&adds);
    free(text);
    return rc;
}

int cbea_install(uint8_t *image, size_t image_len,
                 const uint8_t *pkg, size_t pkg_len,
                 char *log, size_t log_len)
{
    logbuf_t lb = { log, log_len, 0 };
    uint8_t *scratch;
    fat_vol_t v;
    cbea_t c;
    int rc;

    if (log && log_len)
        log[0] = '\0';
    if (!fat_locate(image, image_len, &v)) {
        log_add(&lb, "error: no FAT16 \\Nand Disk volume found in the image\n");
        return -1;
    }
    if (!cbea_parse(pkg, pkg_len, &c, &lb))
        return -1;

    /* Work on a copy so a failed install leaves the image untouched. */
    scratch = malloc(image_len);
    if (!scratch) {
        free(c.entries);
        log_add(&lb, "error: out of memory\n");
        return -1;
    }
    memcpy(scratch, image, image_len);
    fat_locate(scratch, image_len, &v);
    rc = install_into(&v, &c, &lb);
    if (rc == 0)
        memcpy(image, scratch, image_len);
    free(scratch);
    free(c.entries);
    return rc;
}
