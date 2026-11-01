#include "cmcu.h"

#include <string.h>

void cmcu_init(cmcu_state_t *s)
{
    /* Stopped: the ROM's stop finds STATUS below 3 at once at power-on. */
    memset(s, 0, sizeof(*s));
}

uint32_t cmcu_read(const cmcu_state_t *s, uint32_t off)
{
    off &= CMCU_SIZE - 1u;
    if (off == CMCU_STATUS)
        return s->running ? CMCU_STATUS_RUNNING : 0;
    return s->regs[off / 4];
}

void cmcu_write(cmcu_state_t *s, uint32_t off, uint32_t val, uint64_t now_ns)
{
    off &= CMCU_SIZE - 1u & ~3u;
    s->regs[off / 4] = val;
    switch (off) {
    case CMCU_CMD0:
        if ((val & 0xFFu) == 0x5Cu && (s->regs[CMCU_ARG0 / 4] & 0xFFu) == 0x35u)
            s->running = false;
        break;
    case CMCU_CMD:
        if ((val & 0xFFu) == 0x3Au && (s->regs[CMCU_ARG / 4] & 0xFFu) == 0x53u) {
            s->running = true;
            s->reloaded_ns = now_ns;
        }
        break;
    case CMCU_TIMEOUT:
        s->reloaded_ns = now_ns;
        break;
    default:
        break;
    }
}

bool cmcu_expired(cmcu_state_t *s, uint64_t now_ns)
{
    uint64_t seconds = s->regs[CMCU_TIMEOUT / 4] & 0xFFFFu;

    if (!s->running || !seconds || now_ns - s->reloaded_ns < seconds * UINT64_C(1000000000))
        return false;
    s->running = false;
    return true;
}
