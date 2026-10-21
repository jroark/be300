/*
 * Stowaway key map tests: the Fn chord in stowaway_queue_mapped_key(), and
 * the web and Android tables against the canonical src/stowaway_keys.def.
 */

#include "stowaway.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

void be300_stowaway_signal_uart_irq(int pending)
{
    (void)pending;
}

typedef struct {
    const char *code;
    const char *android;
    unsigned value;
} keymap_row_t;

static const keymap_row_t keymap[] = {
#define STOWAWAY_KEY(code, sdl, android, value) { #code, #android, (value) },
#include "stowaway_keys.def"
#undef STOWAWAY_KEY
};

#define KEYMAP_LEN (sizeof(keymap) / sizeof(keymap[0]))

static void connect_dock(void)
{
    stowaway_configure(true);
    stowaway_uart_note_modem_control(true, true);
    CHECK(stowaway_uart_rx_pop() == 0xfa);
    CHECK(stowaway_uart_rx_pop() == 0xfd);
    CHECK(stowaway_uart_rx_count() == 0);
}

static void expect_bytes(const int *bytes, size_t n)
{
    for (size_t i = 0; i < n; i++)
        CHECK(stowaway_uart_rx_pop() == bytes[i]);
    CHECK(stowaway_uart_rx_count() == 0);
}

static void test_fn_chord(void)
{
    connect_dock();

    /* A plain key is one byte each way. */
    CHECK(stowaway_queue_mapped_key(17, false));
    CHECK(stowaway_queue_mapped_key(17, true));
    expect_bytes((const int[]){ 17, 17 | 0x80 }, 2);

    /* Home = Fn + Left: Fn goes down first and comes up last. */
    CHECK(stowaway_queue_mapped_key(STOWAWAY_FN | 81, false));
    CHECK(stowaway_queue_mapped_key(STOWAWAY_FN | 81, true));
    expect_bytes((const int[]){ 8, 81, 81 | 0x80, 8 | 0x80 }, 4);

    /* Overlapping chords share one Fn press. */
    CHECK(stowaway_queue_mapped_key(STOWAWAY_FN | 81, false));
    CHECK(stowaway_queue_mapped_key(STOWAWAY_FN | 83, false));
    CHECK(stowaway_queue_mapped_key(STOWAWAY_FN | 81, true));
    CHECK(stowaway_queue_mapped_key(STOWAWAY_FN | 83, true));
    expect_bytes((const int[]){ 8, 81, 83, 81 | 0x80, 83 | 0x80, 8 | 0x80 },
        6);

    stowaway_configure(false);
    CHECK(!stowaway_queue_mapped_key(STOWAWAY_FN | 81, false));
}

static void test_table_values(void)
{
    for (size_t i = 0; i < KEYMAP_LEN; i++) {
        unsigned scancode = keymap[i].value & ~STOWAWAY_FN;

        /* Stowaway.dll drops scancodes >= 96 (slti at 0x102cd4); 8 is Fn
         * itself and 87 is an empty slot in every layout. */
        CHECK(scancode < 96);
        CHECK(scancode != 8 && scancode != 87);
    }
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long len;
    char *buf;

    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        failures++;
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)len + 1);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(buf);
        failures++;
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

/* "256 + n" or "n" at p; returns the value or -1. */
static long parse_value(const char *p)
{
    char *end;
    long v = strtol(p, &end, 10);

    while (*end == ' ')
        end++;
    if (*end == '+') {
        long n = strtol(end + 1, &end, 10);
        if (v != (long)STOWAWAY_FN)
            return -1;
        v += n;
    }
    return v;
}

/*
 * Walks every "<prefix>NAME<sep>VALUE" entry between begin and end and checks
 * it against the canonical table, by code (web) or Android keycode name.
 */
static void check_table(const char *what, const char *text, const char *begin,
    const char *end, const char *prefix, const char *sep, bool by_android)
{
    const char *start = text ? strstr(text, begin) : NULL;
    const char *stop = start ? strstr(start, end) : NULL;
    bool seen[KEYMAP_LEN] = { false };
    size_t found = 0;

    if (!start || !stop) {
        fprintf(stderr, "%s: table not found\n", what);
        failures++;
        return;
    }
    for (const char *p = strstr(start, prefix); p && p < stop;
         p = strstr(p + 1, prefix)) {
        const char *name = p + strlen(prefix);
        const char *name_end = strstr(name, sep);
        size_t len;
        long value;
        bool matched = false;

        if (!name_end || name_end > stop)
            break;
        len = (size_t)(name_end - name);
        value = parse_value(name_end + strlen(sep));
        for (size_t i = 0; i < KEYMAP_LEN; i++) {
            const char *want = by_android ? keymap[i].android : keymap[i].code;
            if (strlen(want) == len && strncmp(want, name, len) == 0) {
                matched = true;
                seen[i] = true;
                if (value != (long)keymap[i].value) {
                    fprintf(stderr, "%s: %.*s = %ld, stowaway_keys.def has %u\n",
                        what, (int)len, name, value, keymap[i].value);
                    failures++;
                }
            }
        }
        if (!matched) {
            fprintf(stderr, "%s: %.*s is not in stowaway_keys.def\n", what,
                (int)len, name);
            failures++;
        }
        found++;
    }
    for (size_t i = 0; i < KEYMAP_LEN; i++) {
        if (!seen[i]) {
            fprintf(stderr, "%s: missing %s\n", what,
                by_android ? keymap[i].android : keymap[i].code);
            failures++;
        }
    }
    CHECK(found > 0);
}

static void test_front_end_tables(void)
{
    char *js = read_file(BE300_SOURCE_DIR "/web/app.js");
    char *java = read_file(BE300_SOURCE_DIR
        "/android/app/src/main/java/com/jroark/be300/Be300View.java");

    check_table("web/app.js", js, "const STOWAWAY_KEY_CODES = new Map([",
        "]);", "[\"", "\", ", false);
    check_table("Be300View.java", java, "STOWAWAY_KEYS = new SparseIntArray",
        "    }\n", "STOWAWAY_KEYS.put(KeyEvent.KEYCODE_", ", ", true);
    free(js);
    free(java);
}

int main(void)
{
    test_fn_chord();
    test_table_values();
    test_front_end_tables();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("stowaway keymap: ok\n");
    return 0;
}
