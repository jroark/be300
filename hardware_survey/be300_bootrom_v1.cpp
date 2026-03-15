#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

extern "C" BOOL VirtualCopy(LPVOID, LPVOID, DWORD, DWORD);
typedef BOOL (*SETKMODE)(BOOL);
typedef DWORD (*SETPROCPERMISSIONS)(DWORD);

#ifndef PAGE_PHYSICAL
#define PAGE_PHYSICAL 0x40000000
#endif

#ifndef PAGE_NOCACHE
#define PAGE_NOCACHE 0x0200
#endif

#define BOOTROM_BASE_PA     0x1FC00000u
#define BOOTROM_SIZE        0x00004000u
#define BOOTROM_PAGE_SIZE   0x00001000u
#define BOOTROM_PAGE_COUNT  (BOOTROM_SIZE / BOOTROM_PAGE_SIZE)
#define RESET_DUMP_SIZE     0x00000400u
#define BCU_BASE_PA         0x0F000000u
#define BCU_DUMP_SIZE       0x00000020u
#define PASS_COUNT          3

typedef struct {
    BOOL rom_page_ok[BOOTROM_PAGE_COUNT];
    BOOL rom_ok;
    DWORD rom_page_crc32[BOOTROM_PAGE_COUNT];
    DWORD rom_crc32;
    BYTE rom[BOOTROM_SIZE];

    BOOL bcu_ok;
    BYTE bcu[BCU_DUMP_SIZE];
} bootrom_pass_t;

static HANDLE g_hFile = INVALID_HANDLE_VALUE;
static BOOL g_had_error = FALSE;
static bootrom_pass_t g_passes[PASS_COUNT];

static void Log(const char *fmt, ...)
{
    if (g_hFile == INVALID_HANDLE_VALUE)
        return;

    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    {
        int n = _vsnprintf(buffer, sizeof(buffer) - 1, fmt, args);
        if (n < 0)
            buffer[sizeof(buffer) - 1] = '\0';
        else
            buffer[n] = '\0';
    }
    va_end(args);

    {
        DWORD written = 0;
        WriteFile(g_hFile, buffer, (DWORD)strlen(buffer), &written, NULL);
    }
}

static DWORD ReadLE32(const BYTE *p)
{
    return ((DWORD)p[0]) |
           ((DWORD)p[1] << 8) |
           ((DWORD)p[2] << 16) |
           ((DWORD)p[3] << 24);
}

static WORD ReadLE16(const BYTE *p)
{
    return (WORD)(((WORD)p[0]) | ((WORD)p[1] << 8));
}

static DWORD Crc32(const BYTE *data, DWORD size)
{
    DWORD crc = 0xFFFFFFFFu;
    DWORD i = 0;
    for (i = 0; i < size; i++) {
        DWORD j = 0;
        crc ^= (DWORD)data[i];
        for (j = 0; j < 8; j++) {
            if (crc & 1u)
                crc = (crc >> 1) ^ 0xEDB88320u;
            else
                crc >>= 1;
        }
    }
    return ~crc;
}

static void EnableKernelAccess(void)
{
    HMODULE hCore = GetModuleHandle(L"coredll.dll");
    if (!hCore) {
        Log("[PRIV] GetModuleHandle(coredll) failed\r\n");
        g_had_error = TRUE;
        return;
    }

    {
        SETKMODE pSetKMode = (SETKMODE)GetProcAddress(hCore, L"SetKMode");
        if (pSetKMode) {
            BOOL km = pSetKMode(TRUE);
            Log("[PRIV] SetKMode(TRUE) -> %u\r\n", km ? 1u : 0u);
        } else {
            Log("[PRIV] SetKMode not available\r\n");
            g_had_error = TRUE;
        }
    }

    {
        SETPROCPERMISSIONS pSetPerms = (SETPROCPERMISSIONS)GetProcAddress(hCore, L"SetProcPermissions");
        if (pSetPerms) {
            DWORD old_perm = pSetPerms(0xFFFFFFFFu);
            Log("[PRIV] SetProcPermissions(0xFFFFFFFF) old=0x%08X\r\n", old_perm);
        } else {
            Log("[PRIV] SetProcPermissions not available\r\n");
            g_had_error = TRUE;
        }
    }
}

static BOOL ReadPhysicalBytes(DWORD phys_addr, BYTE *out, DWORD size)
{
    DWORD done = 0;
    BOOL ok = TRUE;

    while (done < size) {
        DWORD cur = phys_addr + done;
        DWORD page_base = cur & ~0xFFFu;
        DWORD page_off = cur & 0xFFFu;
        DWORD chunk = size - done;
        BYTE *vptr = NULL;

        if (chunk > (0x1000u - page_off))
            chunk = 0x1000u - page_off;

        vptr = (BYTE *)VirtualAlloc(0, 0x1000, MEM_RESERVE, PAGE_NOACCESS);
        if (!vptr) {
            Log("[MAP_FAIL] PA=0x%08X VirtualAlloc failed err=%lu\r\n", page_base, GetLastError());
            memset(out + done, 0, chunk);
            g_had_error = TRUE;
            ok = FALSE;
            done += chunk;
            continue;
        }

        if (!VirtualCopy(vptr, (LPVOID)(page_base >> 8), 0x1000,
                         PAGE_READONLY | PAGE_NOCACHE | PAGE_PHYSICAL)) {
            Log("[MAP_FAIL] PA=0x%08X VirtualCopy failed err=%lu\r\n", page_base, GetLastError());
            memset(out + done, 0, chunk);
            g_had_error = TRUE;
            ok = FALSE;
            VirtualFree(vptr, 0, MEM_RELEASE);
            done += chunk;
            continue;
        }

        {
            DWORD copied = 0;
            BOOL chunk_fault = FALSE;
            while (copied + 4u <= chunk) {
                DWORD word = 0;
                __try {
                    word = *((volatile DWORD *)(vptr + page_off + copied));
                    out[done + copied + 0] = (BYTE)((word >> 0) & 0xFF);
                    out[done + copied + 1] = (BYTE)((word >> 8) & 0xFF);
                    out[done + copied + 2] = (BYTE)((word >> 16) & 0xFF);
                    out[done + copied + 3] = (BYTE)((word >> 24) & 0xFF);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    Log("[READ_FAULT] PA=0x%08X size=0x%lX code=0x%08lX\r\n",
                        cur + copied, chunk - copied, GetExceptionCode());
                    memset(out + done + copied, 0, chunk - copied);
                    g_had_error = TRUE;
                    ok = FALSE;
                    chunk_fault = TRUE;
                }

                if (chunk_fault)
                    break;
                copied += 4u;
            }

            if (!chunk_fault) {
                while (copied < chunk) {
                    __try {
                        out[done + copied] = *((volatile BYTE *)(vptr + page_off + copied));
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        Log("[READ_FAULT] PA=0x%08X size=0x%lX code=0x%08lX\r\n",
                            cur + copied, chunk - copied, GetExceptionCode());
                        memset(out + done + copied, 0, chunk - copied);
                        g_had_error = TRUE;
                        ok = FALSE;
                        break;
                    }
                    copied++;
                }
            }
        }

        VirtualFree(vptr, 0, MEM_RELEASE);
        done += chunk;
    }

    return ok;
}

static BOOL SafeReadPhysicalBytes(DWORD phys_addr, BYTE *out, DWORD size)
{
    BOOL ok = FALSE;
    __try {
        ok = ReadPhysicalBytes(phys_addr, out, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[READ_FATAL] PA=0x%08X size=0x%lX code=0x%08lX\r\n",
            phys_addr, size, GetExceptionCode());
        memset(out, 0, size);
        g_had_error = TRUE;
        ok = FALSE;
    }
    return ok;
}

static void DumpWords(const BYTE *data, DWORD base_pa, DWORD size)
{
    DWORD off = 0;
    while (off + 16u <= size) {
        DWORD w0 = ReadLE32(data + off + 0);
        DWORD w1 = ReadLE32(data + off + 4);
        DWORD w2 = ReadLE32(data + off + 8);
        DWORD w3 = ReadLE32(data + off + 12);
        Log("0x%08X: %08X %08X %08X %08X\r\n",
            base_pa + off, w0, w1, w2, w3);
        off += 16u;
    }
}

static void CapturePass(int pass_index)
{
    DWORD i = 0;
    bootrom_pass_t *p = &g_passes[pass_index];
    memset(p, 0, sizeof(*p));
    p->rom_ok = TRUE;

    for (i = 0; i < BOOTROM_PAGE_COUNT; i++) {
        DWORD page_off = i * BOOTROM_PAGE_SIZE;
        DWORD page_pa = BOOTROM_BASE_PA + page_off;
        BYTE *page_ptr = p->rom + page_off;
        p->rom_page_ok[i] = SafeReadPhysicalBytes(page_pa, page_ptr, BOOTROM_PAGE_SIZE);
        if (!p->rom_page_ok[i])
            p->rom_ok = FALSE;
        p->rom_page_crc32[i] = Crc32(page_ptr, BOOTROM_PAGE_SIZE);
    }
    p->rom_crc32 = Crc32(p->rom, BOOTROM_SIZE);

    p->bcu_ok = SafeReadPhysicalBytes(BCU_BASE_PA, p->bcu, BCU_DUMP_SIZE);
}

static void LogPassData(int pass_index)
{
    DWORD i = 0;
    bootrom_pass_t *p = &g_passes[pass_index];

    Log("\r\n--- PASS %d ROM SNAPSHOT ---\r\n", pass_index + 1);
    Log("[ROM_RANGE] pass=%d base=0x%08X size=0x%08X status=%s crc32=0x%08X\r\n",
        pass_index + 1,
        BOOTROM_BASE_PA,
        BOOTROM_SIZE,
        p->rom_ok ? "OK" : "PARTIAL",
        p->rom_crc32);

    for (i = 0; i < BOOTROM_PAGE_COUNT; i++) {
        DWORD page_pa = BOOTROM_BASE_PA + (i * BOOTROM_PAGE_SIZE);
        DWORD first_word = ReadLE32(p->rom + (i * BOOTROM_PAGE_SIZE));
        DWORD last_word = ReadLE32(p->rom + (i * BOOTROM_PAGE_SIZE) + BOOTROM_PAGE_SIZE - 4u);
        Log("[ROM_PAGE] pass=%d index=%lu PA=0x%08X size=0x%04X status=%s crc32=0x%08X first=0x%08X last=0x%08X\r\n",
            pass_index + 1,
            i,
            page_pa,
            BOOTROM_PAGE_SIZE,
            p->rom_page_ok[i] ? "OK" : "PARTIAL",
            p->rom_page_crc32[i],
            first_word,
            last_word);
    }

    if (pass_index == 0) {
        Log("\r\n--- PASS 1 ROM FULL DUMP (PA 0x1FC00000..0x1FC03FFF) ---\r\n");
        DumpWords(p->rom, BOOTROM_BASE_PA, BOOTROM_SIZE);
    }

    Log("\r\n--- PASS %d RESET WINDOW DUMP (PA 0x1FC00000..0x1FC003FF) ---\r\n", pass_index + 1);
    DumpWords(p->rom, BOOTROM_BASE_PA, RESET_DUMP_SIZE);

    Log("\r\n--- PASS %d BCU READBACK (PA 0x0F000000..0x0F00001F) ---\r\n", pass_index + 1);
    if (!p->bcu_ok)
        Log("[BCU_RANGE] pass=%d base=0x%08X size=0x%X status=PARTIAL\r\n",
            pass_index + 1, BCU_BASE_PA, BCU_DUMP_SIZE);
    else
        Log("[BCU_RANGE] pass=%d base=0x%08X size=0x%X status=OK\r\n",
            pass_index + 1, BCU_BASE_PA, BCU_DUMP_SIZE);

    DumpWords(p->bcu, BCU_BASE_PA, BCU_DUMP_SIZE);

    {
        WORD romsize = ReadLE16(p->bcu + 0x04);
        WORD romspeed = ReadLE16(p->bcu + 0x06);
        Log("[BCU_FIELD] pass=%d ROMSIZEREG@0x0F000004=0x%04X\r\n", pass_index + 1, romsize);
        Log("[BCU_FIELD] pass=%d ROMSPEEDREG@0x0F000006=0x%04X\r\n", pass_index + 1, romspeed);
    }
}

static void ComparePasses(int from_index, int to_index)
{
    DWORD i = 0;
    DWORD changed_bytes = 0;
    DWORD changed_words = 0;
    DWORD changed_bcu_words = 0;
    DWORD first_change_pa = 0;
    DWORD first_change_before = 0;
    DWORD first_change_after = 0;
    BOOL first_change_seen = FALSE;
    BOOL complete = TRUE;

    bootrom_pass_t *a = &g_passes[from_index];
    bootrom_pass_t *b = &g_passes[to_index];

    for (i = 0; i < BOOTROM_PAGE_COUNT; i++) {
        if (!a->rom_page_ok[i] || !b->rom_page_ok[i])
            complete = FALSE;
    }
    if (!a->bcu_ok || !b->bcu_ok)
        complete = FALSE;

    for (i = 0; i < BOOTROM_SIZE; i++) {
        if (a->rom[i] != b->rom[i]) {
            changed_bytes++;
            if (!first_change_seen) {
                DWORD first_word_off = (i & ~3u);
                first_change_seen = TRUE;
                first_change_pa = BOOTROM_BASE_PA + i;
                first_change_before = ReadLE32(a->rom + first_word_off);
                first_change_after = ReadLE32(b->rom + first_word_off);
            }
        }
    }

    for (i = 0; i + 4u <= BOOTROM_SIZE; i += 4u) {
        DWORD wa = ReadLE32(a->rom + i);
        DWORD wb = ReadLE32(b->rom + i);
        if (wa != wb) {
            changed_words++;
        }
    }

    for (i = 0; i + 4u <= BCU_DUMP_SIZE; i += 4u) {
        DWORD wa = ReadLE32(a->bcu + i);
        DWORD wb = ReadLE32(b->bcu + i);
        if (wa != wb)
            changed_bcu_words++;
    }

    Log("[PASS_COMPARE] from=%d to=%d complete=%u rom_changed_bytes=%lu rom_changed_words=%lu bcu_changed_words=%lu\r\n",
        from_index + 1,
        to_index + 1,
        complete ? 1u : 0u,
        changed_bytes,
        changed_words,
        changed_bcu_words);

    if (first_change_seen) {
        Log("[PASS_COMPARE] from=%d to=%d first_rom_change_PA=0x%08X from_word=0x%08X to_word=0x%08X\r\n",
            from_index + 1,
            to_index + 1,
            first_change_pa,
            first_change_before,
            first_change_after);
    } else {
        Log("[PASS_COMPARE] from=%d to=%d first_rom_change_PA=NONE\r\n",
            from_index + 1,
            to_index + 1);
    }

    for (i = 0; i < BOOTROM_PAGE_COUNT; i++) {
        DWORD pa = BOOTROM_BASE_PA + (i * BOOTROM_PAGE_SIZE);
        Log("[PAGE_COMPARE] from=%d to=%d index=%lu PA=0x%08X from_ok=%u to_ok=%u from_crc32=0x%08X to_crc32=0x%08X same_crc=%u\r\n",
            from_index + 1,
            to_index + 1,
            i,
            pa,
            a->rom_page_ok[i] ? 1u : 0u,
            b->rom_page_ok[i] ? 1u : 0u,
            a->rom_page_crc32[i],
            b->rom_page_crc32[i],
            (a->rom_page_crc32[i] == b->rom_page_crc32[i]) ? 1u : 0u);
    }

    Log("[BCU_COMPARE] from=%d to=%d from_romsize=0x%04X to_romsize=0x%04X from_romspeed=0x%04X to_romspeed=0x%04X\r\n",
        from_index + 1,
        to_index + 1,
        ReadLE16(a->bcu + 0x04),
        ReadLE16(b->bcu + 0x04),
        ReadLE16(a->bcu + 0x06),
        ReadLE16(b->bcu + 0x06));
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nCmdShow)
{
    int pass = 0;
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    g_hFile = CreateFile(L"\\BE300BootROM_v1.txt", GENERIC_WRITE, 0, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_hFile == INVALID_HANDLE_VALUE) {
        MessageBox(NULL, L"Could not create \\BE300BootROM_v1.txt", L"BE300 Boot-ROM Survey v1",
                   MB_OK | MB_ICONHAND);
        return 1;
    }

    __try {
        Log("BE-300 Boot-ROM Survey v1\r\n");
        Log("tick_ms=%lu\r\n", GetTickCount());

        Log("\r\n--- PRIVILEGE BOOTSTRAP ---\r\n");
        EnableKernelAccess();

        Log("\r\n--- CAPTURE PASSES ---\r\n");
        for (pass = 0; pass < PASS_COUNT; pass++) {
            CapturePass(pass);
            LogPassData(pass);
            if (pass + 1 < PASS_COUNT)
                Sleep(200);
        }

        Log("\r\n--- PASS DIFFERENCES ---\r\n");
        ComparePasses(0, 1);
        ComparePasses(1, 2);
        ComparePasses(0, 2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[FATAL] code=0x%08lX\r\n", GetExceptionCode());
        g_had_error = TRUE;
    }

    Log("\r\nsurvey_complete status=%s\r\n", g_had_error ? "PARTIAL_ERROR" : "OK");
    CloseHandle(g_hFile);
    g_hFile = INVALID_HANDLE_VALUE;

    if (g_had_error) {
        MessageBox(NULL,
                   L"BE300 Boot-ROM Survey v1 completed with partial errors.\nSee \\BE300BootROM_v1.txt",
                   L"BE300 Boot-ROM Survey v1",
                   MB_OK | MB_ICONEXCLAMATION);
        return 2;
    }

    MessageBox(NULL,
               L"BE300 Boot-ROM Survey v1 complete.\nSee \\BE300BootROM_v1.txt",
               L"BE300 Boot-ROM Survey v1",
               MB_OK | MB_ICONASTERISK);
    return 0;
}
