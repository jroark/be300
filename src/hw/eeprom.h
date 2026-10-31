/* The BE-300's serial EEPROM (IC3), on the bit-banged bus that eeprom.dll
 * drives (E2pRead 01A210FC, transport 01A212F4): SCL at companion 0xA19C,
 * SDA out at 0xA1A0, SDA in at 0xA1A4, device address 0xA0, a one-byte word
 * address of twice the selector. Selector 0x4A (bytes 0x94/0x95) holds the
 * battery A/D calibration, 0x44 the contrast trim. The factory contents are
 * per unit and unknown, so the model starts erased (0xFF), which makes
 * Windows CE and Glaze use their default calibration. See docs/BATTERY.md. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define EEPROM_SCL_OFF      0xA19Cu
#define EEPROM_SDA_OUT_OFF  0xA1A0u
#define EEPROM_SDA_IN_OFF   0xA1A4u
#define EEPROM_SIZE         256u
#define EEPROM_BATTERY_CAL  0x94u     /* selector 0x4A: max, min bytes */

typedef enum {
    EEPROM_IDLE,        /* waiting for START */
    EEPROM_DEVICE,      /* receiving the device byte */
    EEPROM_WORD,        /* receiving the word address */
    EEPROM_WRITE,       /* receiving data bytes */
    EEPROM_READ,        /* sending data bytes */
} eeprom_phase_t;

typedef struct {
    uint8_t mem[EEPROM_SIZE];
    bool scl, sda_master, sda_slave;
    eeprom_phase_t phase;
    eeprom_phase_t next;    /* after the acknowledge */
    bool ack_slot;          /* ninth clock of a received byte: we acknowledge */
    bool mack_slot;         /* ninth clock of a sent byte: the master does */
    bool master_nack;
    uint8_t shift, bits, addr;
} eeprom_state_t;

void eeprom_init(eeprom_state_t *s);
void eeprom_set_scl(eeprom_state_t *s, bool level);
void eeprom_set_sda(eeprom_state_t *s, bool level);
bool eeprom_sda(const eeprom_state_t *s);
