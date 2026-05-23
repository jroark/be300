#include "win32_compat.h"

#ifdef _WIN32

static int winsock_initialized;

/* If be300.exe was launched from cmd.exe / PowerShell / Windows Terminal,
 * inherit that console so the CLI mode's stdout/stderr/stdin go to the
 * shell that started us instead of vanishing. When launched from Explorer,
 * Start Menu, or a .be300vm file association, no parent console exists and
 * AttachConsole fails — that's the desired GUI-launcher case, and we leave
 * stdio detached rather than popping a fresh console window. */
static void try_attach_parent_console(void)
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return;

    /* freopen_s is MSVC-flavoured; freopen works on every MinGW build. */
    FILE *unused;
    unused = freopen("CONOUT$", "w", stdout); (void)unused;
    unused = freopen("CONOUT$", "w", stderr); (void)unused;
    unused = freopen("CONIN$",  "r", stdin);  (void)unused;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    /* Shell already printed its prompt on the line we now share; emit a
     * leading newline so our first stdout/stderr line isn't glued to it. */
    fputc('\n', stderr);
}

int be300_win32_init(void)
{
    WSADATA wsa;

    if (winsock_initialized)
        return 1;

    try_attach_parent_console();

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "Failed to initialize Winsock\n");
        return 0;
    }
    winsock_initialized = 1;
    return 1;
}

void be300_win32_shutdown(void)
{
    if (!winsock_initialized)
        return;
    WSACleanup();
    winsock_initialized = 0;
}

int be300_clock_gettime(int clock_id, struct timespec *ts)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    FILETIME ft;
    ULARGE_INTEGER uli;
    uint64_t ns;

    if (!ts)
        return -1;

    if (clock_id == CLOCK_MONOTONIC) {
        if (freq.QuadPart == 0 && !QueryPerformanceFrequency(&freq))
            return -1;
        if (!QueryPerformanceCounter(&counter))
            return -1;
        ns = (uint64_t)((counter.QuadPart * 1000000000ull) / freq.QuadPart);
        ts->tv_sec = (time_t)(ns / 1000000000ull);
        ts->tv_nsec = (long)(ns % 1000000000ull);
        return 0;
    }

    GetSystemTimeAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    /* FILETIME is 100 ns ticks since 1601-01-01 UTC. */
    ns = (uli.QuadPart - 116444736000000000ull) * 100ull;
    ts->tv_sec = (time_t)(ns / 1000000000ull);
    ts->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}

int be300_gettimeofday(struct timeval *tv, void *tz)
{
    struct timespec ts;

    (void)tz;
    if (!tv)
        return -1;
    if (be300_clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return -1;
    tv->tv_sec = (long)ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}

int be300_nanosleep(const struct timespec *req, struct timespec *rem)
{
    uint64_t ms;

    (void)rem;
    if (!req)
        return -1;
    ms = (uint64_t)req->tv_sec * 1000ull +
        ((uint64_t)req->tv_nsec + 999999ull) / 1000000ull;
    Sleep((DWORD)ms);
    return 0;
}

unsigned int be300_sleep(unsigned int seconds)
{
    Sleep(seconds * 1000u);
    return 0;
}

int be300_usleep(unsigned int usec)
{
    Sleep((usec + 999u) / 1000u);
    return 0;
}

#endif /* _WIN32 */
