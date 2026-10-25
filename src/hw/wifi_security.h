/* Secured virtual AP fixture. Ethernet payloads represent firmware-decrypted
 * traffic; this is an authenticator/key-installation test, not an RF model. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct {
    unsigned mode, stage;
    bool valid, authorized, pairwise, group;
    uint8_t secret[65]; unsigned secret_len;
    uint8_t mac[6], bssid[6], pmk[32], ptk[64], anonce[32], gtk[32];
    uint8_t ie[80]; unsigned ie_len;
    uint64_t replay;
    uint8_t pending[512]; unsigned pending_len;
} wifi_security_t;
/* mode: open=0, WEP=1, WPA/TKIP=2, WPA2/CCMP=3. */
void wifi_security_init(wifi_security_t *, const uint8_t mac[6]);
bool wifi_security_configure(wifi_security_t *, unsigned mode, const char *key);
void wifi_security_reset(wifi_security_t *);
bool wifi_security_wep(const wifi_security_t *, unsigned index, const uint8_t *, unsigned);
void wifi_security_start(wifi_security_t *);
bool wifi_security_tx(wifi_security_t *, const uint8_t *, size_t);
unsigned wifi_security_pop(wifi_security_t *, uint8_t *, unsigned);
bool wifi_security_key(wifi_security_t *, bool pairwise, unsigned cipher, const uint8_t *, unsigned);

bool wifi_security_rekey(wifi_security_t *);
