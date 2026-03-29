/* Minimal framebuffer test for BE-300 (240x320 16bpp RGB565) */
/* No libc - direct syscalls only, MIPS-II compatible */

#define O_RDWR 2
#define SYS_exit  4001
#define SYS_write 4004
#define SYS_open  4005

static long _syscall3(long nr, long a0, long a1, long a2) {
    register long v0 __asm__("v0") = nr;
    register long a0r __asm__("a0") = a0;
    register long a1r __asm__("a1") = a1;
    register long a2r __asm__("a2") = a2;
    __asm__ volatile("syscall" : "+r"(v0) : "r"(a0r), "r"(a1r), "r"(a2r) : "a3", "memory");
    return v0;
}

static void puts(const char *s) {
    int len = 0;
    while (s[len]) len++;
    _syscall3(SYS_write, 1, (long)s, len);
}

void _start(void) {
    int fd, x, y;
    unsigned short line[240];

    puts("fbtest: opening /dev/fb0\n");
    fd = _syscall3(SYS_open, (long)"/dev/fb0", O_RDWR, 0);
    if (fd < 0) {
        puts("fbtest: cannot open /dev/fb0\n");
        _syscall3(SYS_exit, 1, 0, 0);
    }
    puts("fbtest: drawing color bars\n");

    /* 240x320 at 16bpp RGB565 */
    for (y = 0; y < 320; y++) {
        unsigned short color;
        if (y < 80)       color = 0xF800; /* red */
        else if (y < 160) color = 0x07E0; /* green */
        else if (y < 240) color = 0x001F; /* blue */
        else              color = 0xFFFF; /* white */
        for (x = 0; x < 240; x++) line[x] = color;
        _syscall3(SYS_write, fd, (long)line, 480);
    }

    puts("fbtest: done\n");
    for(;;) ;
}
