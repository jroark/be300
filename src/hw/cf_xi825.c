/* Native Ethernet backend for the XI-825 station controller. */
#include "cf.h"
#include <stdlib.h>
#include <string.h>

static void xi825_tx(void *opaque, const uint8_t *packet, size_t length)
{
    cf_state_t *s = opaque;
    memcpy(s->xi825_nic.mac_address, s->xi825.mac, 6);
    if (s->xi825_net)
        net_ethernet_tx(s->xi825_net, &s->xi825_nic, (unsigned char *)packet,
                        (int)length);
}

void cf_xi825_tick(cf_state_t *s)
{
    if (!s || s->card_kind != CF_CARD_XI825 || !s->attached)
        return;
    s->xi825.opaque = s;
    s->xi825.transmit = xi825_tx;
    xi825_tick(&s->xi825);
    if (!s->xi825_net)
        return;
    /* Leave frames queued in the backend when the card's RX queue is full. */
    for (unsigned i = 0; i < 8 && s->xi825.rx_count < 8; ++i) {
        unsigned char *packet;
        int length;
        if (!net_ethernet_rx_avail(s->xi825_net, &s->xi825_nic) ||
            !net_ethernet_rx(s->xi825_net, &s->xi825_nic, &packet, &length))
            break;
        if (length > 0)
            xi825_receive(&s->xi825, packet, (size_t)length);
        free(packet);
    }
}
