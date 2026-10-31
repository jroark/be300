/* Serial EEPROM on the BE-300's bit-banged bus. See eeprom.h.
 *
 * A two-wire slave at device 0xA0: data changes while SCL is low, START and
 * STOP are SDA edges while SCL is high. Writes go straight to the array;
 * eeprom.dll's E2pWrite timing (page buffer, write cycle) isn't modelled. */
#include "hw/eeprom.h"

#include <string.h>

void eeprom_init(eeprom_state_t *s)
{
    memset(s, 0, sizeof(*s));
    memset(s->mem, 0xFF, sizeof(s->mem));
    /* Idle bus, as Windows CE leaves it (hw_dump_vrc4173.txt 0x0A00A19C). */
    s->scl = s->sda_master = s->sda_slave = true;
}

static void drive_bit(eeprom_state_t *s)
{
    s->sda_slave = (s->mem[s->addr] >> (7 - s->bits)) & 1u;
}

static void byte_received(eeprom_state_t *s)
{
    switch (s->phase) {
    case EEPROM_DEVICE:
        if ((s->shift & 0xFEu) != 0xA0u) {
            s->phase = EEPROM_IDLE;     /* not ours: no acknowledge */
            return;
        }
        s->next = (s->shift & 1u) ? EEPROM_READ : EEPROM_WORD;
        break;
    case EEPROM_WORD:
        s->addr = s->shift;
        s->next = EEPROM_WRITE;
        break;
    case EEPROM_WRITE:
        s->mem[s->addr++] = s->shift;
        s->next = EEPROM_WRITE;
        break;
    default:
        return;
    }
    s->sda_slave = false;
    s->ack_slot = true;
}

void eeprom_set_scl(eeprom_state_t *s, bool level)
{
    if (level == s->scl)
        return;
    s->scl = level;
    if (level) {
        if (s->phase == EEPROM_READ) {
            if (s->mack_slot)
                s->master_nack = s->sda_master;
            else
                s->bits++;
        } else if (s->phase != EEPROM_IDLE && !s->ack_slot && s->bits < 8) {
            s->shift = (uint8_t)((s->shift << 1) | (s->sda_master ? 1u : 0u));
            s->bits++;
        }
        return;
    }

    /* Falling edge: the slave changes SDA only now. */
    if (s->ack_slot) {
        s->ack_slot = false;
        s->sda_slave = true;
        s->phase = s->next;
        s->bits = s->shift = 0;
        if (s->phase == EEPROM_READ)
            drive_bit(s);
    } else if (s->phase == EEPROM_READ) {
        if (s->mack_slot) {
            s->mack_slot = false;
            if (s->master_nack) {
                s->phase = EEPROM_IDLE;
                s->sda_slave = true;
            } else {
                s->addr++;
                s->bits = 0;
                drive_bit(s);
            }
        } else if (s->bits == 8) {
            s->sda_slave = true;
            s->mack_slot = true;
        } else {
            drive_bit(s);
        }
    } else if (s->phase != EEPROM_IDLE && s->bits == 8) {
        byte_received(s);
    }
}

void eeprom_set_sda(eeprom_state_t *s, bool level)
{
    if (level == s->sda_master)
        return;
    s->sda_master = level;
    if (!s->scl)
        return;
    s->ack_slot = s->mack_slot = false;
    s->sda_slave = true;
    s->bits = s->shift = 0;
    s->phase = level ? EEPROM_IDLE : EEPROM_DEVICE;    /* STOP : START */
}

bool eeprom_sda(const eeprom_state_t *s)
{
    return s->sda_master && s->sda_slave;
}
