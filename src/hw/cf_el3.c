/* Native Ethernet backend for the 3c589 model (el3.c). */
#include "cf.h"
#include <stdlib.h>
#include <string.h>

static void el3_tx(void *opaque, const uint8_t *packet, size_t length)
{
    cf_state_t *s = opaque;
    if (s->el3_net)
        net_ethernet_tx(s->el3_net, &s->el3_nic, (unsigned char *)packet, (int)length);
}

void cf_el3_tick(cf_state_t *s)
{
    if (!s || s->card_kind != CF_CARD_3C589 || !s->attached)
        return;
    s->el3.opaque = s;
    s->el3.transmit = el3_tx;
    if (!s->el3_net)
        return;
    /* Leave frames queued in the backend while the card's RX FIFO is full. */
    for (unsigned i = 0; i < 8 && !el3_rx_full(&s->el3); ++i) {
        unsigned char *packet;
        int length;
        if (!net_ethernet_rx_avail(s->el3_net, &s->el3_nic) ||
            !net_ethernet_rx(s->el3_net, &s->el3_nic, &packet, &length))
            break;
        if (length > 0)
            el3_receive(&s->el3, packet, (size_t)length);
        free(packet);
    }
}
