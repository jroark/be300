/* CompactFlash ATA taskfile: data width after SET FEATURES 01h / 81h. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hw/cf.h"

/* The card models link against the network layer; nothing here uses it. */
void net_generate_unique_mac(struct machine *m, unsigned char *p) { (void)m; memset(p, 2, 6); }
void net_add_nic(struct net *n, struct nic_data *nic) { (void)n; (void)nic; }
int net_ethernet_rx_avail(struct net *n, struct nic_data *nic) { (void)n; (void)nic; return 0; }
int net_ethernet_rx(struct net *n, struct nic_data *nic, unsigned char **p, int *len)
{ (void)n; (void)nic; (void)p; (void)len; return 0; }
void net_ethernet_tx(struct net *n, struct nic_data *nic, unsigned char *p, int len)
{ (void)n; (void)nic; (void)p; (void)len; }
int net_ether_eq(const uint8_t *a, const uint8_t *b) { return !memcmp(a, b, 6); }
int net_ether_multicast(const uint8_t *a) { return a[0] & 1; }
int net_ether_broadcast(const uint8_t *a) { static const uint8_t b[6] = {255,255,255,255,255,255}; return !memcmp(a, b, 6); }

#define TF 0xC1F0u /* primary taskfile in the card I/O window */

static void command(cf_state_t *s, uint8_t features, uint8_t cmd)
{
    cf_window_write(s, TF + 1, 1, features);
    cf_window_write(s, TF + 2, 1, 1);     /* one sector */
    cf_window_write(s, TF + 3, 1, 0);     /* LBA 0 */
    cf_window_write(s, TF + 4, 1, 0);
    cf_window_write(s, TF + 5, 1, 0);
    cf_window_write(s, TF + 6, 1, 0xE0);
    cf_window_write(s, TF + 7, 1, cmd);
}

int main(void)
{
    static cf_state_t s;
    char path[] = "/tmp/be300_cf_ata_XXXXXX";
    uint8_t image[64 * 512];
    int fd = mkstemp(path);

    assert(fd >= 0);
    for (size_t i = 0; i < sizeof(image); i++)
        image[i] = (uint8_t)(i * 7u + 1u);
    assert(write(fd, image, sizeof(image)) == (ssize_t)sizeof(image));
    close(fd);

    cf_init(&s);
    assert(cf_load_image(&s, path) == 0);
    unlink(path);

    /* Default: a 16-bit access carries two bytes. */
    command(&s, 0, 0x20);
    assert(cf_window_read(&s, TF, 2) == (uint64_t)(image[0] | image[1] << 8));
    for (int i = 2; i < 512; i += 2)
        assert(cf_window_read(&s, TF, 2) == (uint64_t)(image[i] | image[i + 1] << 8));
    assert(!(cf_window_read(&s, TF + 7, 1) & 0x08));          /* DRQ gone */

    /* SET FEATURES 01h: one byte per access whatever its width, D15-D8 zero. */
    command(&s, 0x01, 0xEF);
    assert(!(cf_window_read(&s, TF + 7, 1) & 0x01));
    command(&s, 0, 0x20);
    for (int i = 0; i < 512; i++)
        assert(cf_window_read(&s, TF, 2) == image[i]);
    assert(!(cf_window_read(&s, TF + 7, 1) & 0x08));

    /* Writes move one byte per access too. */
    command(&s, 0, 0x30);
    for (int i = 0; i < 512; i++)
        cf_window_write(&s, TF, 2, 0xAA00u | (uint8_t)i);
    assert(!(cf_window_read(&s, TF + 7, 1) & 0x08));
    command(&s, 0, 0x20);
    for (int i = 0; i < 512; i++)
        assert(cf_window_read(&s, TF, 1) == (uint8_t)i);

    /* 81h turns it off again; so does a soft reset. */
    command(&s, 0x81, 0xEF);
    command(&s, 0, 0x20);
    assert(cf_window_read(&s, TF, 2) == 0x0100u);
    command(&s, 0x01, 0xEF);
    cf_window_write(&s, 0xC3F6u, 1, 0x04);
    cf_window_write(&s, 0xC3F6u, 1, 0x00);
    command(&s, 0, 0x20);
    assert(cf_window_read(&s, TF, 2) == 0x0100u);

    puts("test_cf_ata: ok");
    return 0;
}
