/*
 * Offline .cbea installer test (src/cbea_install.c).
 *
 * Builds a restore-image-style partition table plus a small FAT16 volume
 * in memory, installs synthetic packages laid out like WMP 1.1's
 * wmp1.1.cbea, and reads the result back.
 */

#include "cbea_install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
            #cond); \
        failures++; \
    } \
} while (0)

#define PART_LBA      32u
#define PART_SECTORS  8192u
#define FAT_SECTORS   33u
#define NFATS         2u
#define ROOT_ENTRIES  256u
#define IMAGE_SIZE    ((PART_LBA + PART_SECTORS) * 512u)

static uint8_t *g_img;
static char g_log[16384];

static void wr16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, unsigned v) { wr16(p, v & 0xFFFF); wr16(p + 2, v >> 16); }
static unsigned rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }

static void build_image(void)
{
    uint8_t *v;

    g_img = calloc(1, IMAGE_SIZE);
    /* Block-0 table: 8 x 0xFF, start, count (entry 3 = FAT16). */
    memset(g_img, 0xFF, 0x40);
    for (int i = 0; i < 4; i++) {
        wr32(g_img + i * 16 + 8, i == 3 ? PART_LBA : 1u + i);
        wr32(g_img + i * 16 + 12, i == 3 ? PART_SECTORS : 1u);
    }
    v = g_img + PART_LBA * 512u;
    v[0] = 0xEB; v[1] = 0xFE; v[2] = 0x90;
    wr16(v + 0x0B, 512);
    v[0x0D] = 1;
    wr16(v + 0x0E, 1);
    v[0x10] = NFATS;
    wr16(v + 0x11, ROOT_ENTRIES);
    wr16(v + 0x13, PART_SECTORS);
    v[0x15] = 0xF0;
    wr16(v + 0x16, FAT_SECTORS);
    memcpy(v + 0x36, "FAT16   ", 8);
    v[0x1FE] = 0x55; v[0x1FF] = 0xAA;
    for (unsigned f = 0; f < NFATS; f++) {
        uint8_t *fat = v + 512u + f * FAT_SECTORS * 512u;
        wr16(fat, 0xFFF0);
        wr16(fat + 2, 0xFFFF);
    }
}

typedef struct { const char *name; const void *data; size_t len; } pkg_file_t;

static uint8_t *build_cbea(const pkg_file_t *files, unsigned n, size_t *out_len)
{
    size_t total = 0x100;
    uint8_t *buf, *p;

    for (unsigned i = 0; i < n; i++)
        total += 8 + strlen(files[i].name) + 1 + 4 + files[i].len;
    buf = calloc(1, total);
    wr16(buf, 0x0100);
    wr32(buf + 2, n);
    p = buf + 0x100;
    for (unsigned i = 0; i < n; i++) {
        size_t nl = strlen(files[i].name);
        wr32(p, (unsigned)(4 + nl + 1 + 4 + files[i].len));
        p[6] = 3;
        p[7] = (uint8_t)nl;
        memcpy(p + 8, files[i].name, nl);
        p += 8 + nl + 1;
        wr32(p, (unsigned)files[i].len);
        memcpy(p + 4, files[i].data, files[i].len);
        p += 4 + files[i].len;
    }
    *out_len = total;
    return buf;
}

static int install(const pkg_file_t *files, unsigned n)
{
    size_t len;
    uint8_t *pkg = build_cbea(files, n, &len);
    int rc = cbea_install(g_img, IMAGE_SIZE, pkg, len, g_log, sizeof(g_log));
    free(pkg);
    if (rc != 0)
        fprintf(stderr, "install log:\n%s", g_log);
    return rc;
}

static int file_matches(const char *path, const void *data, size_t len)
{
    size_t got = 0;
    uint8_t *buf = cbea_fat_read_file(g_img, IMAGE_SIZE, path, &got);
    int ok = buf && got == len && memcmp(buf, data, len) == 0;
    if (!ok)
        fprintf(stderr, "mismatch: %s (got %s, %zu bytes)\n", path, buf ? "file" : "nothing", got);
    free(buf);
    return ok;
}

/* Find value `name` under (root, key) in System.reg; returns its data. */
static const uint8_t *reg_find(const uint8_t *reg, size_t len, unsigned root,
                               const char *key, const char *name,
                               unsigned *type, unsigned *dlen, int *key_count)
{
    size_t pos = 8;
    int in_key = 0;
    const uint8_t *hit = NULL;

    *key_count = 0;
    while (pos + 4 <= len) {
        unsigned plen = rd16(reg + pos), rt = rd16(reg + pos + 2);
        const uint8_t *r = reg + pos;
        if (rt == 1) {
            unsigned nc = rd16(r + 6);
            int eq = rd16(r + 4) == root && nc == strlen(key) + 1;
            for (unsigned i = 0; eq && i < nc - 1; i++)
                eq = rd16(r + 10 + i * 2) == (unsigned char)key[i];
            in_key = eq;
            *key_count += eq;
        } else if (in_key) {
            unsigned nc = rd16(r + 6);
            int eq = nc == strlen(name) + 1;
            for (unsigned i = 0; eq && i < nc - 1; i++)
                eq = rd16(r + 10 + i * 2) == (unsigned char)name[i];
            if (eq) {
                *type = rd16(r + 4);
                *dlen = rd16(r + 8);
                hit = r + 10 + nc * 2;
            }
        }
        pos += 4 + plen;
    }
    return hit;
}

static const char k_inf1[] =
    "; test package\r\n"
    "[App Information]\r\n"
    "Program     = \"Test App\"\r\n"
    "InstallDir  = \"?Drive?\\Program Files\\Test1.1\"\r\n"
    "Destination = MAINMEM_ONLY\r\n"
    "Version     = 1.1\r\n"
    "ExeFile     = \"\\Program Disk\\Test1.1\\Test.exe\"\r\n"
    "Unload      = 1\r\n"
    "Uninstall   = \"UnTest.inf\"\r\n"
    "Caption     = \"Test Caption\"\r\n"
    "\r\n"
    "[Copy Files]\r\n"
    "\"?Drive?\\Program Files\\UnTest.inf\" , \"UnTest.inf\", COPYFLG_REPLACEONLY\r\n"
    "\"%INSTALL_DIR%\\Windows Media.exe.cpk\" , \"Windows Media.exe.cpk\", COPYFLG_REPLACEONLY\r\n"
    "\"%INSTALL_DIR%\\Windows Mediaplayer.cpk\" , \"big.bin\", COPYFLG_REPLACEONLY\r\n"
    "\"%INSTALL_DIR%\\super.bmp\" , \"super.bmp\", COPYFLG_REPLACEONLY\r\n"
    "\"\\Nand Disk\\My Documents\\Temp\\App-install.inf\" , \"App-install.cfg\", COPYFLG_REPLACEONLY\r\n"
    "\"\\Windows\\helper.dll\" , \"helper.dll\", COPYFLG_REPLACEONLY\r\n"
    "\r\n"
    "[Add Registry]\r\n"
    "HKEY_LOCAL_MACHINE, \"SOFTWARE\\Test\", , REG_SZ, FLG_ADDREG_REPLACEONLY, \" \"\r\n"
    "HKEY_LOCAL_MACHINE, \"SOFTWARE\\Test\\Buttons\", \"Left\", REG_DWORD, FLG_ADDREG_REPLACEONLY, 37\r\n"
    "HKEY_LOCAL_MACHINE, \"SOFTWARE\\Test\\Parameters\", \"SkinDir\", REG_SZ, FLG_ADDREG_REPLACEONLY, \"Program Disk\\test\\\\\"\r\n"
    "HKEY_CLASSES_ROOT, \".tst\", , REG_SZ, FLG_ADDREG_REPLACEONLY, \"tstfile\"\r\n"
    "; End\r\n";

static const char k_inf2[] =
    "[App Information]\r\n"
    "InstallDir  = \"?Drive?\\Program Files\\Test1.1\"\r\n"
    "Destination = ALTERNATIVE\r\n"
    "[Copy Files]\r\n"
    "\"%INSTALL_DIR%\\super.bmp\" , \"super2.bmp\", COPYFLG_REPLACEONLY\r\n"
    "[Add Registry]\r\n"
    "HKEY_LOCAL_MACHINE, \"SOFTWARE\\Test\\Buttons\", \"Left\", REG_DWORD, FLG_ADDREG_REPLACEONLY, 25\r\n"
    "HKEY_LOCAL_MACHINE, \"SOFTWARE\\Test\\Buttons\", \"Right\", REG_DWORD, FLG_ADDREG_REPLACEONLY, 27\r\n"
    "HKEY_CURRENT_USER, \"Software\\Other\", \"Build\", REG_DWORD, FLG_ADDREG_REPLACEONLY, 00000000\r\n";

static const char k_inf_storage[] =
    "[App Information]\r\n"
    "InstallDir  = \"?Drive?\\Program Files\\X\"\r\n"
    "Destination = STORAGE_ONLY\r\n"
    "[Copy Files]\r\n"
    "\"%INSTALL_DIR%\\x.bin\" , \"x.bin\", COPYFLG_REPLACEONLY\r\n";

static const char k_inf_bad_dest[] =
    "[App Information]\r\n"
    "InstallDir  = \"\\Storage Card\\Program Files\\X\"\r\n"
    "[Copy Files]\r\n"
    "\"%INSTALL_DIR%\\x.bin\" , \"x.bin\", COPYFLG_REPLACEONLY\r\n";

int main(void)
{
    static uint8_t big[5000];
    uint8_t *before;
    size_t reg_len = 0;
    uint8_t *reg;
    unsigned type = 0, dlen = 0;
    int keys = 0;
    const uint8_t *val;
    const uint8_t *vol;

    for (size_t i = 0; i < sizeof(big); i++)
        big[i] = (uint8_t)(i * 7 + 3);
    build_image();

    {
        const pkg_file_t files[] = {
            { "Install.inf", k_inf1, sizeof(k_inf1) - 1 },
            { "UnTest.inf", "uninstall", 9 },
            { "Windows Media.exe.cpk", "CASIO.$&%-Z payload", 19 },
            { "big.bin", big, sizeof(big) },
            { "super.bmp", "BM-one", 6 },
            { "App-install.cfg", "{GUID}", 6 },
            { "helper.dll", "MZ", 2 },
        };
        CHECK(install(files, 7) == 0);
    }
    CHECK(file_matches("Program Files\\UnTest.inf", "uninstall", 9));
    CHECK(file_matches("Program Files\\Test1.1\\Windows Media.exe.cpk", "CASIO.$&%-Z payload", 19));
    CHECK(file_matches("program files\\test1.1\\WINDOWS MEDIAPLAYER.CPK", big, sizeof(big)));
    CHECK(file_matches("Program Files\\Test1.1\\super.bmp", "BM-one", 6));
    CHECK(file_matches("My Documents\\Temp\\App-install.inf", "{GUID}", 6));
    CHECK(file_matches("Program Files\\Patch\\helper.dll", "MZ", 2));
    /* Short-name tails for the two colliding long names. */
    CHECK(file_matches("Program Files\\Test1.1\\WINDOW~1.CPK", "CASIO.$&%-Z payload", 19));
    CHECK(file_matches("Program Files\\Test1.1\\WINDOW~2.CPK", big, sizeof(big)));

    vol = g_img + PART_LBA * 512u;
    CHECK(memcmp(vol + 512, vol + 512 + FAT_SECTORS * 512, FAT_SECTORS * 512) == 0);

    reg = cbea_fat_read_file(g_img, IMAGE_SIZE, "Backup\\System.reg", &reg_len);
    CHECK(reg != NULL);
    CHECK(file_matches("Backup\\System.$$$", reg, reg_len));
    if (reg) {
        CHECK(memcmp(reg, "\xb2\x74\x83\x1d", 4) == 0);
        CHECK(rd32(reg + 4) == reg_len);
        val = reg_find(reg, reg_len, 2, "SOFTWARE\\Test", "Default", &type, &dlen, &keys);
        CHECK(val && type == 1 && dlen == 4 && rd16(val) == ' ' && keys == 1);
        val = reg_find(reg, reg_len, 2, "SOFTWARE\\Test\\Buttons", "Left", &type, &dlen, &keys);
        CHECK(val && type == 4 && dlen == 4 && rd32(val) == 0x37);
        val = reg_find(reg, reg_len, 2, "SOFTWARE\\Test\\Parameters", "SkinDir", &type, &dlen, &keys);
        CHECK(val && type == 1 && dlen == 2 * 19);
        CHECK(val && rd16(val + 2 * 17) == '\\' && rd16(val + 2 * 18) == 0);
        val = reg_find(reg, reg_len, 0, ".tst", "Default", &type, &dlen, &keys);
        CHECK(val && type == 1);
        /* Launcher entry, shaped like nand/beshell.bin's installed apps. */
        val = reg_find(reg, reg_len, 2, "SOFTWARE\\MENU\\Test App", "ANAME", &type, &dlen, &keys);
        CHECK(val && type == 1 && dlen == 2 * 13 && rd16(val) == 'T' && keys == 1);
        val = reg_find(reg, reg_len, 2, "SOFTWARE\\MENU\\Test App", "APATH", &type, &dlen, &keys);
        CHECK(val && dlen == 2 * 31);
        val = reg_find(reg, reg_len, 2, "SOFTWARE\\MENU\\Test App", "APPNO", &type, &dlen, &keys);
        CHECK(val && type == 4 && rd32(val) == 8);
        val = reg_find(reg, reg_len, 2, "SOFTWARE\\MENU\\Test App", "ULOAD", &type, &dlen, &keys);
        CHECK(val && rd32(val) == 1);
        val = reg_find(reg, reg_len, 2, "SOFTWARE\\MENU\\Test App", "ICON", &type, &dlen, &keys);
        CHECK(val && type == 1 && dlen == 2);
        val = reg_find(reg, reg_len, 2, "SOFTWARE\\MENU\\Test App\\VERSION", "MINOR", &type, &dlen, &keys);
        CHECK(val && rd32(val) == 1);
        free(reg);
    }

    /* Second package: replace a file, merge into the existing snapshot. */
    {
        const pkg_file_t files[] = {
            { "Install.inf", k_inf2, sizeof(k_inf2) - 1 },
            { "super2.bmp", "BM-two-longer", 13 },
        };
        CHECK(install(files, 2) == 0);
    }
    CHECK(file_matches("Program Files\\Test1.1\\super.bmp", "BM-two-longer", 13));
    reg = cbea_fat_read_file(g_img, IMAGE_SIZE, "Backup\\System.reg", &reg_len);
    CHECK(reg != NULL);
    if (reg) {
        val = reg_find(reg, reg_len, 2, "SOFTWARE\\Test\\Buttons", "Left", &type, &dlen, &keys);
        CHECK(val && rd32(val) == 0x25 && keys == 1);
        val = reg_find(reg, reg_len, 2, "SOFTWARE\\Test\\Buttons", "Right", &type, &dlen, &keys);
        CHECK(val && rd32(val) == 0x27);
        val = reg_find(reg, reg_len, 1, "Software\\Other", "Build", &type, &dlen, &keys);
        CHECK(val && rd32(val) == 0);
        val = reg_find(reg, reg_len, 2, "SOFTWARE\\Test\\Parameters", "SkinDir", &type, &dlen, &keys);
        CHECK(val != NULL);
        free(reg);
    }

    /* Failures leave the image untouched. */
    before = malloc(IMAGE_SIZE);
    memcpy(before, g_img, IMAGE_SIZE);
    {
        const pkg_file_t files[] = {
            { "Install.inf", k_inf_storage, sizeof(k_inf_storage) - 1 },
            { "x.bin", "x", 1 },
        };
        CHECK(install(files, 2) != 0);
        CHECK(strstr(g_log, "STORAGE_ONLY") != NULL);
    }
    {
        const pkg_file_t files[] = {
            { "Install.inf", k_inf_bad_dest, sizeof(k_inf_bad_dest) - 1 },
            { "x.bin", "x", 1 },
        };
        CHECK(install(files, 2) != 0);
    }
    {
        const pkg_file_t files[] = { { "Install.inf", k_inf1, sizeof(k_inf1) - 1 } };
        CHECK(install(files, 1) != 0); /* payloads missing */
        CHECK(strstr(g_log, "missing") != NULL);
    }
    {
        size_t len;
        const pkg_file_t files[] = {
            { "Install.inf", k_inf2, sizeof(k_inf2) - 1 },
            { "super2.bmp", "BM", 2 },
        };
        uint8_t *pkg = build_cbea(files, 2, &len);
        CHECK(cbea_install(g_img, IMAGE_SIZE, pkg, len - 1, g_log, sizeof(g_log)) != 0);
        free(pkg);
    }
    CHECK(memcmp(before, g_img, IMAGE_SIZE) == 0);
    free(before);

    free(g_img);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("cbea_install: all checks passed\n");
    return 0;
}
