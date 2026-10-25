/* Ethernet edge of the Marvell station service; same gateway as XI-825. */
#include "cf.h"
#include <stdlib.h>

static void wl54cf_tx(void *opaque, const uint8_t *packet, size_t length)
{
    cf_state_t *s = opaque;
    if (s->wl54cf_net)
        net_ethernet_tx(s->wl54cf_net, &s->wl54cf_nic, (unsigned char *)packet, (int)length);
}
void cf_wl54cf_tick(cf_state_t *s)
{
    if (!s || !s->attached || s->card_kind != CF_CARD_WL54CF)
        return;
    s->wl54cf.opaque = s;
    s->wl54cf.transmit = wl54cf_tx;
    if (!s->wl54cf_net || !s->wl54cf.associated)
        return;
    for (unsigned i = 0; i < WL54CF_QUEUE && s->wl54cf.rx_count < WL54CF_QUEUE; i++) {
        unsigned char *p;
        int n;
        if (!net_ethernet_rx_avail(s->wl54cf_net, &s->wl54cf_nic) ||
            !net_ethernet_rx(s->wl54cf_net, &s->wl54cf_nic, &p, &n))
            break;
        if (n > 0)
            wl54cf_receive(&s->wl54cf, p, (size_t)n);
        free(p);
    }
}
