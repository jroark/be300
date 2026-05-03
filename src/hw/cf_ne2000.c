/*
 *  src/hw/cf_ne2000.c — NE2000 PCMCIA card emulation.
 *
 *  Implements the NE2000 register banks, RX/TX ring buffer, and the
 *  10 Mbit ISA-PCMCIA window observed at PA 0x0A00C000-0x0A00C400 once
 *  pcmcia.dll programs the card I/O window.
 *
 *  Split out of src/hw/cf.c. Public API (declared in hw/cf.h):
 *    cf_ne2000_tick.
 *  Private helpers consumed by cf.c:
 *    ne2000_seed_prom, ne2000_reset, ne2000_window_read,
 *    ne2000_window_write (all declared in cf_internal.h).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cf.h"
#include "cf_internal.h"
#include "net.h"

#ifdef __EMSCRIPTEN__
int  be300_web_net_bridge_enabled(void);
void be300_web_net_tx_enqueue(const uint8_t *packet, uint32_t len);
#endif

void ne2000_seed_prom(cf_state_t *s)
{
    uint8_t prom[16];

    memset(prom, 0, sizeof(prom));
    memcpy(prom, s->ne2000.par, sizeof(s->ne2000.par));
    prom[14] = 0x57u;
    prom[15] = 0x57u;

    memset(s->ne2000.mem, 0, sizeof(s->ne2000.mem));
    for (size_t i = 0; i < sizeof(prom); i++) {
        s->ne2000.mem[i * 2u + 0u] = prom[i];
        s->ne2000.mem[i * 2u + 1u] = prom[i];
    }
}

void ne2000_reset(cf_state_t *s)
{
    s->ne2000.cr = NE_CR_STP | NE_CR_RD2;
    s->ne2000.pstart = 0x40u;
    s->ne2000.pstop = 0x80u;
    s->ne2000.bnry = 0x40u;
    s->ne2000.tpsr = 0x20u;
    s->ne2000.tbcr0 = 0;
    s->ne2000.tbcr1 = 0;
    s->ne2000.isr = NE_ISR_RST;
    s->ne2000.rsar0 = 0;
    s->ne2000.rsar1 = 0;
    s->ne2000.rbcr0 = 0;
    s->ne2000.rbcr1 = 0;
    s->ne2000.rcr = 0;
    s->ne2000.tcr = 0;
    s->ne2000.dcr = NE_DCR_WTS;
    s->ne2000.imr = 0;
    s->ne2000.curr = 0x41u;
    s->ne2000.rsr = 0;
    s->ne2000.tsr = 0;
    s->ne2000.reset_latch = 0;
    s->ne2000.remote_cmd = NE_CR_RD2;
    s->ne2000.remote_addr = 0;
    s->ne2000.remote_count = 0;
    ne2000_seed_prom(s);
}

static uint8_t ne2000_mem_read(const cf_state_t *s, uint16_t addr)
{
    return s->ne2000.mem[addr % CF_NE2000_MEM_SIZE];
}

static void ne2000_mem_write(cf_state_t *s, uint16_t addr, uint8_t value)
{
    if (addr < CF_NE2000_MEM_SIZE)
        s->ne2000.mem[addr] = value;
}

static uint16_t ne2000_remote_count(const cf_state_t *s)
{
    return (uint16_t)s->ne2000.rbcr0 | ((uint16_t)s->ne2000.rbcr1 << 8);
}

static void ne2000_remote_done_if_needed(cf_state_t *s)
{
    if (s->ne2000.remote_count == 0 &&
        s->ne2000.remote_cmd != NE_CR_RD2)
        s->ne2000.isr |= NE_ISR_RDC;
}

static void ne2000_update_mac(cf_state_t *s)
{
    memcpy(s->ne2000.nic.mac_address, s->ne2000.par,
        sizeof(s->ne2000.nic.mac_address));
}

static void ne2000_transmit(cf_state_t *s)
{
    uint16_t len = (uint16_t)s->ne2000.tbcr0 |
        ((uint16_t)s->ne2000.tbcr1 << 8);
    uint16_t addr = (uint16_t)s->ne2000.tpsr << 8;
    uint8_t packet[1600];
    bool bridged = false;

    if (len > sizeof(packet))
        len = sizeof(packet);
    for (uint16_t i = 0; i < len; i++)
        packet[i] = ne2000_mem_read(s, (uint16_t)(addr + i));

#ifdef __EMSCRIPTEN__
    if (len >= 14 && be300_web_net_bridge_enabled()) {
        be300_web_net_tx_enqueue(packet, len);
        bridged = true;
    }
#endif

    if (!bridged && s->ne2000.net && len >= 14)
        net_ethernet_tx(s->ne2000.net, &s->ne2000.nic, packet, len);

    s->ne2000.tsr = NE_TSR_PTX;
    s->ne2000.isr |= NE_ISR_PTX;
    s->ne2000.cr &= (uint8_t)~NE_CR_TXP;
}

static bool ne2000_accept_packet(const cf_state_t *s, const uint8_t *packet,
                                 int len)
{
    if (!s || !packet || len < 14)
        return false;
    if (!(s->ne2000.cr & NE_CR_STA) || (s->ne2000.cr & NE_CR_STP))
        return false;
    if (s->ne2000.rcr & NE_RCR_MON)
        return false;
    if (net_ether_broadcast(packet))
        return (s->ne2000.rcr & NE_RCR_AB) != 0;
    if (net_ether_multicast(packet))
        return (s->ne2000.rcr & (NE_RCR_AM | NE_RCR_PRO)) != 0;
    return (s->ne2000.rcr & NE_RCR_PRO) ||
        net_ether_eq(packet, s->ne2000.par);
}

static uint8_t ne2000_next_page(const cf_state_t *s, uint8_t page,
                                uint8_t pages)
{
    uint8_t pstart = s->ne2000.pstart ? s->ne2000.pstart : 0x40u;
    uint8_t pstop = s->ne2000.pstop > pstart ? s->ne2000.pstop : 0x80u;
    unsigned next = (unsigned)page + pages;

    while (next >= pstop)
        next = (unsigned)pstart + (next - pstop);
    return (uint8_t)next;
}

static bool ne2000_ring_has_space(const cf_state_t *s, uint8_t pages)
{
    uint8_t pstart = s->ne2000.pstart ? s->ne2000.pstart : 0x40u;
    uint8_t pstop = s->ne2000.pstop > pstart ? s->ne2000.pstop : 0x80u;
    uint8_t curr = s->ne2000.curr;
    uint8_t bnry = s->ne2000.bnry;
    unsigned free_pages;

    if (curr < pstart || curr >= pstop)
        curr = pstart;
    if (bnry < pstart || bnry >= pstop)
        bnry = pstart;

    if (curr <= bnry)
        free_pages = (unsigned)(bnry - curr);
    else
        free_pages = (unsigned)(pstop - curr) + (unsigned)(bnry - pstart);

    return free_pages > pages;
}

static void ne2000_ring_write(cf_state_t *s, uint16_t off, uint8_t value)
{
    uint16_t pstart = (uint16_t)(s->ne2000.pstart ? s->ne2000.pstart : 0x40u) << 8;
    uint16_t pstop = (uint16_t)(s->ne2000.pstop > s->ne2000.pstart ?
        s->ne2000.pstop : 0x80u) << 8;
    uint16_t addr = off;

    if (addr < pstart || addr >= pstop)
        addr = (uint16_t)(pstart + ((addr - pstart) % (pstop - pstart)));
    ne2000_mem_write(s, addr, value);
}

static void ne2000_receive_packet(cf_state_t *s, const uint8_t *packet, int len)
{
    uint16_t wire_len;
    uint16_t total;
    uint8_t pages;
    uint8_t curr;
    uint8_t next;
    uint16_t base;

    if (!ne2000_accept_packet(s, packet, len))
        return;

    wire_len = (uint16_t)len + 4u;
    total = (uint16_t)(wire_len + 4u);
    pages = (uint8_t)((total + 255u) >> 8);
    if (pages == 0)
        pages = 1;
    if (!ne2000_ring_has_space(s, pages)) {
        s->ne2000.isr |= NE_ISR_OVW;
        return;
    }

    curr = s->ne2000.curr;
    if (curr < s->ne2000.pstart || curr >= s->ne2000.pstop)
        curr = s->ne2000.pstart;
    next = ne2000_next_page(s, curr, pages);
    base = (uint16_t)curr << 8;

    ne2000_ring_write(s, base + 0u, NE_RSR_PRX |
        (net_ether_eq(packet, s->ne2000.par) ? NE_RSR_PHY : 0u));
    ne2000_ring_write(s, base + 1u, next);
    ne2000_ring_write(s, base + 2u, (uint8_t)(wire_len & 0xFFu));
    ne2000_ring_write(s, base + 3u, (uint8_t)(wire_len >> 8));
    for (int i = 0; i < len; i++)
        ne2000_ring_write(s, (uint16_t)(base + 4u + (uint16_t)i), packet[i]);
    for (uint16_t i = 0; i < 4u; i++)
        ne2000_ring_write(s, (uint16_t)(base + 4u + (uint16_t)len + i), 0);

    s->ne2000.curr = next;
    s->ne2000.rsr = NE_RSR_PRX;
    s->ne2000.isr |= NE_ISR_PRX;
}

void cf_ne2000_tick(cf_state_t *s)
{
    if (!cf_is_ne2000(s) || !s->ne2000.net)
        return;

    for (unsigned i = 0; i < 4; i++) {
        unsigned char *packet = NULL;
        int len = 0;

        if (!net_ethernet_rx_avail(s->ne2000.net, &s->ne2000.nic))
            break;
        if (!net_ethernet_rx(s->ne2000.net, &s->ne2000.nic, &packet, &len))
            break;
        ne2000_receive_packet(s, packet, len);
        free(packet);
    }
}

static uint8_t ne2000_read_reg(cf_state_t *s, uint8_t reg)
{
    uint8_t page = (uint8_t)((s->ne2000.cr & NE_CR_PS_MASK) >> 6);

    reg &= 0x0Fu;
    if (reg == 0)
        return s->ne2000.cr;

    if (page == 1) {
        if (reg >= 1 && reg <= 6)
            return s->ne2000.par[reg - 1u];
        if (reg == 7)
            return s->ne2000.curr;
        if (reg >= 8)
            return s->ne2000.mar[reg - 8u];
        return 0;
    }

    if (page == 2) {
        switch (reg) {
        case 1: return s->ne2000.pstart;
        case 2: return s->ne2000.pstop;
        case 3: return s->ne2000.remote_addr & 0xFFu;
        case 4: return s->ne2000.tpsr;
        case 5: return s->ne2000.remote_count & 0xFFu;
        case 6: return s->ne2000.remote_count >> 8;
        case 7: return s->ne2000.curr;
        default: return 0;
        }
    }

    switch (reg) {
    case 3: return s->ne2000.bnry;
    case 4: return s->ne2000.tsr;
    case 7: return s->ne2000.isr;
    case 8: return s->ne2000.remote_addr & 0xFFu;
    case 9: return s->ne2000.remote_addr >> 8;
    case 12: return s->ne2000.rsr;
    case 13:
    case 14:
    case 15:
        return 0;
    default:
        return 0;
    }
}

static uint64_t ne2000_data_read(cf_state_t *s, unsigned size)
{
    uint64_t val = 0;

    for (unsigned i = 0; i < size; i++) {
        uint8_t byte = ne2000_mem_read(s, s->ne2000.remote_addr);
        val |= (uint64_t)byte << (8u * i);
        s->ne2000.remote_addr++;
        if (s->ne2000.remote_count > 0)
            s->ne2000.remote_count--;
    }
    ne2000_remote_done_if_needed(s);
    return val;
}

static void ne2000_data_write(cf_state_t *s, unsigned size, uint64_t value)
{
    for (unsigned i = 0; i < size; i++) {
        ne2000_mem_write(s, s->ne2000.remote_addr,
            (uint8_t)((value >> (8u * i)) & 0xFFu));
        s->ne2000.remote_addr++;
        if (s->ne2000.remote_count > 0)
            s->ne2000.remote_count--;
    }
    ne2000_remote_done_if_needed(s);
}

static void ne2000_write_cr(cf_state_t *s, uint8_t value)
{
    uint8_t old_page = s->ne2000.cr & NE_CR_PS_MASK;

    s->ne2000.cr = value;
    s->ne2000.remote_cmd = value & NE_CR_RD_MASK;
    if (value & NE_CR_STP)
        s->ne2000.isr |= NE_ISR_RST;
    if (value & NE_CR_STA)
        s->ne2000.isr &= (uint8_t)~NE_ISR_RST;
    if ((value & NE_CR_RD_MASK) == NE_CR_RD0 ||
        (value & NE_CR_RD_MASK) == NE_CR_RD1) {
        s->ne2000.remote_addr = (uint16_t)s->ne2000.rsar0 |
            ((uint16_t)s->ne2000.rsar1 << 8);
        s->ne2000.remote_count = ne2000_remote_count(s);
        ne2000_remote_done_if_needed(s);
    }
    if (value & NE_CR_TXP)
        ne2000_transmit(s);
    s->ne2000.cr = (s->ne2000.cr & (uint8_t)~NE_CR_PS_MASK) |
        (value & NE_CR_PS_MASK);
    (void)old_page;
}

static void ne2000_write_reg(cf_state_t *s, uint8_t reg, uint8_t value)
{
    uint8_t page = (uint8_t)((s->ne2000.cr & NE_CR_PS_MASK) >> 6);

    reg &= 0x0Fu;
    if (reg == 0) {
        ne2000_write_cr(s, value);
        return;
    }

    if (page == 1) {
        if (reg >= 1 && reg <= 6) {
            s->ne2000.par[reg - 1u] = value;
            ne2000_update_mac(s);
        } else if (reg == 7) {
            s->ne2000.curr = value;
        } else if (reg >= 8) {
            s->ne2000.mar[reg - 8u] = value;
        }
        return;
    }

    switch (reg) {
    case 1: s->ne2000.pstart = value; break;
    case 2: s->ne2000.pstop = value; break;
    case 3: s->ne2000.bnry = value; break;
    case 4: s->ne2000.tpsr = value; break;
    case 5: s->ne2000.tbcr0 = value; break;
    case 6: s->ne2000.tbcr1 = value; break;
    case 7: s->ne2000.isr &= (uint8_t)~value; break;
    case 8: s->ne2000.rsar0 = value; break;
    case 9: s->ne2000.rsar1 = value; break;
    case 10: s->ne2000.rbcr0 = value; break;
    case 11: s->ne2000.rbcr1 = value; break;
    case 12:
        s->ne2000.rcr = value;
        s->ne2000.nic.promiscuous_mode = (value & NE_RCR_PRO) != 0;
        break;
    case 13: s->ne2000.tcr = value; break;
    case 14: s->ne2000.dcr = value; break;
    case 15: s->ne2000.imr = value; break;
    default: break;
    }
}

uint64_t ne2000_window_read(cf_state_t *s, uint32_t offset, unsigned size)
{
    uint64_t val = 0;
    uint8_t port = (uint8_t)(offset & 0x1Fu);

    if (port == 0x10u)
        return ne2000_data_read(s, size);
    if (port == NE_RESET_PORT) {
        s->ne2000.reset_latch = 1;
        s->ne2000.isr |= NE_ISR_RST;
        return size >= 4 ? UINT32_C(0xFFFFFFFF) : UINT64_C(0xFF);
    }

    for (unsigned i = 0; i < size; i++) {
        uint8_t b = ne2000_read_reg(s, (uint8_t)(port + i));
        val |= (uint64_t)b << (8u * i);
    }
    return val;
}

void ne2000_window_write(cf_state_t *s, uint32_t offset, unsigned size,
                         uint64_t value)
{
    uint8_t port = (uint8_t)(offset & 0x1Fu);

    if (port == 0x10u) {
        ne2000_data_write(s, size, value);
        return;
    }
    if (port == NE_RESET_PORT) {
        s->ne2000.reset_latch = 0;
        s->ne2000.isr |= NE_ISR_RST;
        return;
    }

    for (unsigned i = 0; i < size; i++)
        ne2000_write_reg(s, (uint8_t)(port + i),
            (uint8_t)((value >> (8u * i)) & 0xFFu));
}
