// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements IPv6 networking functions for the Ethernet stack.

#include "netstack_ipv6.h"
#include "ethernet.h"
#include "netstack.h"
#include "netstack_tcp.h"
#include "arch/x86_64/scheduler/scheduler.hpp"

extern "C" void* memcpy(void*, const void*, unsigned long long);
extern "C" void* memset(void*, int, unsigned long long);

namespace {

constexpr uint16_t kEtherTypeIpv6 = 0x86DD;
constexpr uint16_t kIpv6HeaderSize = 40;
constexpr uint8_t kNextHeaderTcp = 6;
constexpr uint8_t kNextHeaderIcmpv6 = 58;
constexpr uint8_t kIcmpRouterSolicitation = 133;
constexpr uint8_t kIcmpRouterAdvertisement = 134;
constexpr uint8_t kIcmpNeighborSolicitation = 135;
constexpr uint8_t kIcmpNeighborAdvertisement = 136;
constexpr uint8_t kNeighborCount = 8;
constexpr uint64_t kRouterWaitTicks = 10;
constexpr uint64_t kNeighborWaitTicks = 100;
constexpr uint64_t kDuplicateAddressWaitTicks = 100;
constexpr uint8_t kMacLength = 6;

struct Neighbor {
    bool valid;
    uint8_t address[16];
    uint8_t mac[6];
};

struct Ipv6State {
    bool initialized;
    bool has_global;
    bool has_prefix;
    bool has_router;
    uint8_t mac[6];
    uint8_t link_local[16];
    uint8_t global[16];
    uint8_t prefix[16];
    uint8_t router[16];
    uint8_t tentative_address[16];
    uint64_t prefix_expiry;
    uint64_t router_expiry;
    uint64_t dad_expiry;
    bool dad_pending;
    bool dad_failed;
    uint16_t timer_poll_count;
    Neighbor neighbors[kNeighborCount];
    uint8_t replace_index;
};

Ipv6State g_ipv6{};

uint16_t read_be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

void write_be16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value >> 8);
    p[1] = static_cast<uint8_t>(value);
}

void write_be32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>(value >> 16);
    p[2] = static_cast<uint8_t>(value >> 8);
    p[3] = static_cast<uint8_t>(value);
}

void copy_bytes(uint8_t* dst, const uint8_t* src, uint16_t size) {
    memcpy(dst, src, size);
}

bool equal_bytes(const uint8_t* a, const uint8_t* b, uint16_t size) {
    for (uint16_t i = 0; i < size; ++i) if (a[i] != b[i]) return false;
    return true;
}

bool is_zero(const uint8_t address[16]) {
    for (uint8_t i = 0; i < 16; ++i) if (address[i]) return false;
    return true;
}

bool is_multicast(const uint8_t address[16]) { return address[0] == 0xFF; }

bool is_link_local(const uint8_t address[16]) {
    return address[0] == 0xFE && (address[1] & 0xC0) == 0x80;
}

bool is_our_address(const uint8_t address[16]) {
    return equal_bytes(address, g_ipv6.link_local, 16) ||
           (g_ipv6.has_global && equal_bytes(address, g_ipv6.global, 16));
}

uint64_t lifetime_deadline(uint32_t seconds) {
    if (seconds == UINT32_MAX) return UINT64_MAX;
    return welovec::x86_64::scheduler::tick_count() +
           static_cast<uint64_t>(seconds) * 100;
}

void refresh_timers() {
    if (!g_ipv6.initialized) return;
    const uint64_t now = welovec::x86_64::scheduler::tick_count();
    if (g_ipv6.has_router && g_ipv6.router_expiry != UINT64_MAX &&
        now >= g_ipv6.router_expiry) {
        g_ipv6.has_router = false;
    }
    if (g_ipv6.has_prefix && g_ipv6.prefix_expiry != UINT64_MAX &&
        now >= g_ipv6.prefix_expiry) {
        g_ipv6.has_prefix = false;
        g_ipv6.has_global = false;
        g_ipv6.dad_pending = false;
        g_ipv6.dad_failed = false;
    }
    if (g_ipv6.dad_pending && now >= g_ipv6.dad_expiry) {
        g_ipv6.dad_pending = false;
        if (!g_ipv6.dad_failed && g_ipv6.has_prefix) {
            copy_bytes(g_ipv6.global, g_ipv6.tentative_address, 16);
            g_ipv6.has_global = true;
        }
    }
}

uint16_t checksum_add(uint32_t sum, const uint8_t* data, uint32_t length) {
    for (uint32_t i = 0; i + 1 < length; i += 2)
        sum += read_be16(data + i);
    if (length & 1) sum += static_cast<uint32_t>(data[length - 1]) << 8;
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return static_cast<uint16_t>(sum);
}

uint16_t icmpv6_checksum(const uint8_t source[16], const uint8_t destination[16],
                         const uint8_t* payload, uint16_t length) {
    uint32_t sum = 0;
    sum = checksum_add(sum, source, 16);
    sum = checksum_add(sum, destination, 16);
    uint8_t tail[8] = {};
    write_be32(tail, length);
    tail[7] = kNextHeaderIcmpv6;
    sum = checksum_add(sum, tail, sizeof(tail));
    sum = checksum_add(sum, payload, length);
    return static_cast<uint16_t>(~sum);
}

void cache_neighbor(const uint8_t address[16], const uint8_t mac[6]) {
    if (is_zero(address) || is_multicast(address)) return;
    for (uint8_t i = 0; i < kNeighborCount; ++i) {
        if (g_ipv6.neighbors[i].valid &&
            equal_bytes(g_ipv6.neighbors[i].address, address, 16)) {
            copy_bytes(g_ipv6.neighbors[i].mac, mac, kMacLength);
            return;
        }
    }
    Neighbor* entry = &g_ipv6.neighbors[g_ipv6.replace_index++ % kNeighborCount];
    entry->valid = true;
    copy_bytes(entry->address, address, 16);
    copy_bytes(entry->mac, mac, kMacLength);
}

bool lookup_neighbor(const uint8_t address[16], uint8_t mac[6]) {
    for (uint8_t i = 0; i < kNeighborCount; ++i) {
        if (g_ipv6.neighbors[i].valid &&
            equal_bytes(g_ipv6.neighbors[i].address, address, 16)) {
            copy_bytes(mac, g_ipv6.neighbors[i].mac, kMacLength);
            return true;
        }
    }
    return false;
}

void make_solicited_node(const uint8_t target[16], uint8_t address[16], uint8_t mac[6]) {
    memset(address, 0, 16);
    address[0] = 0xFF;
    address[1] = 0x02;
    address[11] = 0x01;
    address[12] = 0xFF;
    address[13] = target[13];
    address[14] = target[14];
    address[15] = target[15];
    mac[0] = 0x33;
    mac[1] = 0x33;
    mac[2] = 0xFF;
    mac[3] = target[13];
    mac[4] = target[14];
    mac[5] = target[15];
}

void make_slaac_address(const uint8_t prefix[16], uint8_t address[16]) {
    copy_bytes(address, prefix, 16);
    address[8] = static_cast<uint8_t>(g_ipv6.mac[0] ^ 0x02);
    address[9] = g_ipv6.mac[1];
    address[10] = g_ipv6.mac[2];
    address[11] = 0xFF;
    address[12] = 0xFE;
    address[13] = g_ipv6.mac[3];
    address[14] = g_ipv6.mac[4];
    address[15] = g_ipv6.mac[5];
}

void send_duplicate_address_solicitation(const uint8_t target[16]) {
    uint8_t destination[16];
    uint8_t destination_mac[6];
    make_solicited_node(target, destination, destination_mac);
    uint8_t solicitation[24] = {};
    solicitation[0] = kIcmpNeighborSolicitation;
    copy_bytes(&solicitation[8], target, 16);
    const uint8_t unspecified[16] = {};
    write_be16(&solicitation[2], icmpv6_checksum(unspecified, destination,
                                                  solicitation,
                                                  sizeof(solicitation)));
    (void)netstack_ipv6_send_packet(destination_mac, unspecified, destination,
                                    kNextHeaderIcmpv6, 255,
                                    solicitation, sizeof(solicitation));
}

void start_duplicate_address_detection(const uint8_t prefix[16]) {
    copy_bytes(g_ipv6.prefix, prefix, 16);
    memset(&g_ipv6.prefix[8], 0, 8);
    g_ipv6.has_prefix = true;
    g_ipv6.has_global = false;
    g_ipv6.dad_failed = false;
    make_slaac_address(g_ipv6.prefix, g_ipv6.tentative_address);
    g_ipv6.dad_pending = true;
    g_ipv6.dad_expiry = welovec::x86_64::scheduler::tick_count() +
                        kDuplicateAddressWaitTicks;
    send_duplicate_address_solicitation(g_ipv6.tentative_address);
}

bool prefix_matches(const uint8_t address[16]) {
    return g_ipv6.has_prefix && equal_bytes(address, g_ipv6.prefix, 8);
}

void handle_router_advertisement(const uint8_t source[16], const uint8_t* icmp,
                                 uint16_t length, const uint8_t source_mac[6]) {
    if (length < 16 || !is_link_local(source)) return;
    const uint16_t router_lifetime = read_be16(&icmp[6]);
    if (router_lifetime) {
        copy_bytes(g_ipv6.router, source, 16);
        g_ipv6.has_router = true;
        g_ipv6.router_expiry = lifetime_deadline(router_lifetime);
        cache_neighbor(source, source_mac);
    } else if (g_ipv6.has_router && equal_bytes(g_ipv6.router, source, 16)) {
        g_ipv6.has_router = false;
        g_ipv6.router_expiry = 0;
    }

    uint16_t offset = 16;
    while (offset + 2 <= length) {
        const uint8_t type = icmp[offset];
        const uint8_t units = icmp[offset + 1];
        const uint16_t option_length = static_cast<uint16_t>(units) * 8;
        if (!units || offset + option_length > length) return;
        if (type == 1 && option_length >= 8) {
            cache_neighbor(source, &icmp[offset + 2]);
        } else if (type == 3 && option_length == 32 &&
                   icmp[offset + 2] == 64 && (icmp[offset + 3] & 0x40)) {
            const uint32_t valid_lifetime = read_be32(&icmp[offset + 4]);
            if (valid_lifetime) {
                const uint8_t* advertised_prefix = &icmp[offset + 16];
                const bool same_prefix = g_ipv6.has_prefix &&
                    equal_bytes(g_ipv6.prefix, advertised_prefix, 8);
                g_ipv6.prefix_expiry = lifetime_deadline(valid_lifetime);
                if (!same_prefix) {
                    start_duplicate_address_detection(advertised_prefix);
                } else if (!g_ipv6.has_global && !g_ipv6.dad_pending &&
                           !g_ipv6.dad_failed) {
                    start_duplicate_address_detection(advertised_prefix);
                }
            }
            else if (g_ipv6.has_prefix &&
                     equal_bytes(g_ipv6.prefix, &icmp[offset + 16], 8)) {
                g_ipv6.has_global = false;
                g_ipv6.has_prefix = false;
                g_ipv6.dad_pending = false;
                g_ipv6.dad_failed = false;
            }
        }
        offset = static_cast<uint16_t>(offset + option_length);
    }
}

void send_neighbor_advertisement(const ethernet_packet* packet,
                                 const uint8_t source[16],
                                 const uint8_t target[16]) {
    uint8_t destination[16];
    uint8_t destination_mac[6];
    const bool dad = is_zero(source);
    if (dad) {
        memset(destination, 0, 16);
        destination[0] = 0xFF;
        destination[1] = 0x02;
        destination[15] = 1;
        destination_mac[0] = 0x33;
        destination_mac[1] = 0x33;
        destination_mac[2] = 0;
        destination_mac[3] = 0;
        destination_mac[4] = 0;
        destination_mac[5] = 1;
    } else {
        copy_bytes(destination, source, 16);
        copy_bytes(destination_mac, packet->source, 6);
    }

    uint8_t advertisement[32] = {};
    advertisement[0] = kIcmpNeighborAdvertisement;
    write_be32(&advertisement[4], dad ? 0x20000000u : 0x60000000u);
    copy_bytes(&advertisement[8], target, 16);
    advertisement[24] = 2;
    advertisement[25] = 1;
    copy_bytes(&advertisement[26], g_ipv6.mac, 6);
    write_be16(&advertisement[2], icmpv6_checksum(target, destination,
                                                   advertisement, sizeof(advertisement)));
    (void)netstack_ipv6_send_packet(destination_mac, target, destination,
                                    kNextHeaderIcmpv6, 255,
                                    advertisement, sizeof(advertisement));
}

void send_neighbor_solicitation(const uint8_t target[16]) {
    uint8_t destination[16];
    uint8_t destination_mac[6];
    make_solicited_node(target, destination, destination_mac);
    uint8_t solicitation[32] = {};
    solicitation[0] = kIcmpNeighborSolicitation;
    copy_bytes(&solicitation[8], target, 16);
    solicitation[24] = 1;
    solicitation[25] = 1;
    copy_bytes(&solicitation[26], g_ipv6.mac, 6);
    write_be16(&solicitation[2], icmpv6_checksum(g_ipv6.link_local,
                                                  destination, solicitation,
                                                  sizeof(solicitation)));
    (void)netstack_ipv6_send_packet(destination_mac, g_ipv6.link_local,
                                    destination, kNextHeaderIcmpv6, 255,
                                    solicitation, sizeof(solicitation));
}

void send_router_solicitation() {
    uint8_t destination[16] = {};
    destination[0] = 0xFF;
    destination[1] = 0x02;
    destination[15] = 2;
    const uint8_t destination_mac[6] = {0x33, 0x33, 0, 0, 0, 2};
    uint8_t solicitation[16] = {};
    solicitation[0] = kIcmpRouterSolicitation;
    solicitation[8] = 1;
    solicitation[9] = 1;
    copy_bytes(&solicitation[10], g_ipv6.mac, 6);
    write_be16(&solicitation[2], icmpv6_checksum(g_ipv6.link_local,
                                                  destination, solicitation,
                                                  sizeof(solicitation)));
    (void)netstack_ipv6_send_packet(destination_mac, g_ipv6.link_local,
                                    destination, kNextHeaderIcmpv6, 255,
                                    solicitation, sizeof(solicitation));
}

void handle_icmpv6(const ethernet_packet* packet, const uint8_t source[16],
                   const uint8_t destination[16], const uint8_t* payload,
                   uint16_t length, uint8_t hop_limit) {
    if (length < 4 || icmpv6_checksum(source, destination, payload, length) != 0)
        return;
    switch (payload[0]) {
        case kIcmpRouterSolicitation:
            break;
        case kIcmpRouterAdvertisement:
            if (hop_limit == 255 && payload[1] == 0)
                handle_router_advertisement(source, payload, length, packet->source);
            break;
        case kIcmpNeighborSolicitation: {
            if (hop_limit != 255 || payload[1] != 0 || length < 24) break;
            const uint8_t* target = &payload[8];
            if (g_ipv6.dad_pending &&
                equal_bytes(target, g_ipv6.tentative_address, 16)) {
                g_ipv6.dad_pending = false;
                g_ipv6.dad_failed = true;
                break;
            }
            if (!is_our_address(target)) break;
            if (!is_zero(source)) cache_neighbor(source, packet->source);
            send_neighbor_advertisement(packet, source, target);
            break;
        }
        case kIcmpNeighborAdvertisement: {
            if (hop_limit != 255 || payload[1] != 0 || length < 24) break;
            const uint8_t* target = &payload[8];
            if (g_ipv6.dad_pending &&
                equal_bytes(target, g_ipv6.tentative_address, 16)) {
                g_ipv6.dad_pending = false;
                g_ipv6.dad_failed = true;
                break;
            }
            uint16_t offset = 24;
            while (offset + 2 <= length) {
                const uint8_t type = payload[offset];
                const uint8_t units = payload[offset + 1];
                const uint16_t option_length = static_cast<uint16_t>(units) * 8;
                if (!units || offset + option_length > length) return;
                if (type == 2 && option_length >= 8) {
                    cache_neighbor(target, &payload[offset + 2]);
                    break;
                }
                offset = static_cast<uint16_t>(offset + option_length);
            }
            if (offset == 24) cache_neighbor(target, packet->source);
            break;
        }
        default:
            break;
    }
}

} // namespace

bool netstack_ipv6_send_packet(const uint8_t destination_mac[6],
                               const uint8_t source[16],
                               const uint8_t destination[16],
                               uint8_t next_header, uint8_t hop_limit,
                               const void* payload, uint16_t payload_length) {
    if (!destination_mac || !source || !destination ||
        (!payload && payload_length) ||
        payload_length > ETHERNET_MAX_PAYLOAD_SIZE - kIpv6HeaderSize)
        return false;
    uint8_t packet[ETHERNET_MAX_PAYLOAD_SIZE];
    memset(packet, 0, kIpv6HeaderSize);
    packet[0] = 0x60;
    write_be16(&packet[4], payload_length);
    packet[6] = next_header;
    packet[7] = hop_limit;
    copy_bytes(&packet[8], source, 16);
    copy_bytes(&packet[24], destination, 16);
    if (payload_length) memcpy(&packet[kIpv6HeaderSize], payload, payload_length);
    return ethernet_send(destination_mac, kEtherTypeIpv6,
                         packet, static_cast<uint16_t>(kIpv6HeaderSize + payload_length));
}

void netstack_ipv6_init(const uint8_t mac[6]) {
    memset(&g_ipv6, 0, sizeof(g_ipv6));
    if (!mac) return;
    g_ipv6.initialized = true;
    copy_bytes(g_ipv6.mac, mac, 6);
    g_ipv6.link_local[0] = 0xFE;
    g_ipv6.link_local[1] = 0x80;
    g_ipv6.link_local[8] = static_cast<uint8_t>(mac[0] ^ 0x02);
    g_ipv6.link_local[9] = mac[1];
    g_ipv6.link_local[10] = mac[2];
    g_ipv6.link_local[11] = 0xFF;
    g_ipv6.link_local[12] = 0xFE;
    g_ipv6.link_local[13] = mac[3];
    g_ipv6.link_local[14] = mac[4];
    g_ipv6.link_local[15] = mac[5];

    send_router_solicitation();
}

void netstack_ipv6_handle_packet(const ethernet_packet* packet) {
    if (!packet || !g_ipv6.initialized || packet->payload_length < kIpv6HeaderSize)
        return;
    const uint8_t* ip = packet->payload;
    if ((ip[0] >> 4) != 6) return;
    const uint16_t payload_length = read_be16(&ip[4]);
    if (static_cast<uint32_t>(payload_length) + kIpv6HeaderSize > packet->payload_length)
        return;
    const uint8_t* source = &ip[8];
    const uint8_t* destination = &ip[24];
    const uint8_t next_header = ip[6];
    const uint8_t hop_limit = ip[7];
    const uint8_t* payload = &ip[kIpv6HeaderSize];
    if (next_header == kNextHeaderIcmpv6) {
        handle_icmpv6(packet, source, destination, payload, payload_length, hop_limit);
    } else if (next_header == kNextHeaderTcp && is_our_address(destination)) {
        cache_neighbor(source, packet->source);
        tcp_handle_packet(packet);
    }
}

void netstack_ipv6_poll() {
    if (!g_ipv6.initialized ||
        (!g_ipv6.has_router && !g_ipv6.has_prefix && !g_ipv6.dad_pending)) return;
    const uint16_t interval = g_ipv6.dad_pending ? 32 : 1000;
    if (++g_ipv6.timer_poll_count < interval) return;
    g_ipv6.timer_poll_count = 0;
    refresh_timers();
}

void netstack_ipv6_get_info(netstack_ipv6_info* info) {
    if (!info) return;
    refresh_timers();
    memset(info, 0, sizeof(*info));
    info->configured = g_ipv6.initialized;
    info->has_global_address = g_ipv6.has_global;
    info->has_default_router = g_ipv6.has_router;
    copy_bytes(info->link_local, g_ipv6.link_local, 16);
    copy_bytes(info->global_address, g_ipv6.global, 16);
    copy_bytes(info->prefix, g_ipv6.prefix, 16);
    copy_bytes(info->default_router, g_ipv6.router, 16);
}

bool netstack_ipv6_resolve(const uint8_t destination[16],
                           uint8_t source[16], uint8_t out_mac[6]) {
    refresh_timers();
    if (!destination || !source || !out_mac || !g_ipv6.initialized ||
        is_zero(destination) || is_multicast(destination)) return false;
    while (g_ipv6.dad_pending &&
           welovec::x86_64::scheduler::tick_count() < g_ipv6.dad_expiry) {
        netstack_poll();
        (void)welovec::x86_64::scheduler::sleep(1);
    }
    refresh_timers();
    if (!is_link_local(destination) && !g_ipv6.has_global) return false;
    const bool on_link = is_link_local(destination) || prefix_matches(destination);
    const uint8_t* next_hop = destination;
    if (!on_link) {
        for (uint8_t attempt = 0; !g_ipv6.has_router && attempt < 3; ++attempt) {
            send_router_solicitation();
            const uint64_t deadline = welovec::x86_64::scheduler::tick_count() +
                                      kRouterWaitTicks;
            while (!g_ipv6.has_router &&
                   welovec::x86_64::scheduler::tick_count() < deadline) {
                netstack_poll();
                (void)welovec::x86_64::scheduler::sleep(1);
            }
        }
        if (!g_ipv6.has_router) return false;
        next_hop = g_ipv6.router;
    }
    if (is_link_local(destination)) copy_bytes(source, g_ipv6.link_local, 16);
    else if (g_ipv6.has_global) copy_bytes(source, g_ipv6.global, 16);
    else copy_bytes(source, g_ipv6.link_local, 16);

    if (lookup_neighbor(next_hop, out_mac)) return true;
    for (uint8_t attempt = 0; attempt < 3; ++attempt) {
        send_neighbor_solicitation(next_hop);
        const uint64_t deadline = welovec::x86_64::scheduler::tick_count() +
                                  kNeighborWaitTicks;
        while (welovec::x86_64::scheduler::tick_count() < deadline) {
            netstack_poll();
            if (lookup_neighbor(next_hop, out_mac)) return true;
            (void)welovec::x86_64::scheduler::sleep(1);
        }
    }
    return false;
}

bool netstack_ipv6_parse(const char* text, uint8_t out_address[16]) {
    if (!text || !out_address || !*text) return false;
    uint8_t parsed[16] = {};
    uint8_t group_count = 0;
    int compression = -1;
    const char* cursor = text;
    if (*cursor == ':') {
        if (cursor[1] != ':') return false;
        compression = 0;
        cursor += 2;
        if (!*cursor) { memcpy(out_address, parsed, 16); return true; }
    }
    while (*cursor) {
        if (group_count >= 8) return false;
        uint16_t value = 0;
        uint8_t digits = 0;
        while (*cursor && *cursor != ':') {
            const char c = *cursor++;
            uint8_t nibble;
            if (c >= '0' && c <= '9') nibble = static_cast<uint8_t>(c - '0');
            else if (c >= 'a' && c <= 'f') nibble = static_cast<uint8_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') nibble = static_cast<uint8_t>(c - 'A' + 10);
            else return false;
            if (++digits > 4) return false;
            value = static_cast<uint16_t>((value << 4) | nibble);
        }
        if (!digits) return false;
        parsed[group_count * 2] = static_cast<uint8_t>(value >> 8);
        parsed[group_count * 2 + 1] = static_cast<uint8_t>(value);
        ++group_count;
        if (!*cursor) break;
        ++cursor;
        if (*cursor == ':') {
            if (compression >= 0) return false;
            compression = group_count;
            ++cursor;
            if (!*cursor) break;
        } else if (!*cursor) {
            return false;
        }
    }
    if (compression >= 0) {
        if (group_count >= 8) return false;
        const uint8_t zeros = static_cast<uint8_t>(8 - group_count);
        for (int i = group_count - 1; i >= compression; --i) {
            parsed[(i + zeros) * 2] = parsed[i * 2];
            parsed[(i + zeros) * 2 + 1] = parsed[i * 2 + 1];
        }
        for (uint8_t i = 0; i < zeros; ++i) {
            parsed[(compression + i) * 2] = 0;
            parsed[(compression + i) * 2 + 1] = 0;
        }
    } else if (group_count != 8) {
        return false;
    }
    memcpy(out_address, parsed, 16);
    return true;
}
