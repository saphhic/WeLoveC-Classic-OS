// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements protocols and operations for the network stack.

//written by saphhic.
#include "netstack.h"
#include "debug.h"
#include "ethernet.h"
#include "netstack_ipv6.h"
#include "netstack_tcp.h"
#include "platform.h"
#include "arch/x86_64/scheduler/scheduler.h"

extern "C" void* memcpy(void* destination, const void* source, unsigned long long size);
extern "C" void* memset(void* destination, int value, unsigned long long size);

namespace {

static void format_u32(uint32_t value, char* buffer) {
    char reverse[10];
    uint8_t length = 0;
    do {
        reverse[length++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && length < sizeof(reverse));

    uint8_t index = 0;
    while (length != 0) buffer[index++] = reverse[--length];
    buffer[index] = '\0';
}

void uprint_ip(const uint8_t ip[4]) {
    char buf[16];
    char num_buf[4];
    buf[0] = '\0';

    for (int i = 0; i < 4; ++i) {
        format_u32(ip[i], num_buf);
        // This is a simplified strcat
        char* p = buf;
        while (*p) p++;
        char* s = num_buf;
        while (*s) *p++ = *s++;
        *p = '\0';

        if (i < 3) {
            p = buf;
            while (*p) p++;
            *p++ = '.';
            *p = '\0';
        }
    }
    uprint(buf);
}


constexpr uint8_t kBroadcastMac[ETHERNET_ADDRESS_LENGTH] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};
constexpr uint8_t kIpv4Broadcast[4] = {255, 255, 255, 255};
constexpr uint8_t kZeroIpv4[4] = {0, 0, 0, 0};

constexpr uint16_t kEtherTypeArp = 0x0806;
constexpr uint16_t kEtherTypeIpv4 = 0x0800;
constexpr uint16_t kArpHardwareTypeEthernet = 0x0001;
constexpr uint16_t kArpOperationRequest = 0x0001;
constexpr uint16_t kArpOperationReply = 0x0002;
constexpr uint8_t kIpv4ProtocolIcmp = 0x01;
constexpr uint8_t kIpv4ProtocolUdp = 0x11;
constexpr uint8_t kIpv4ProtocolTcp = 0x06;
constexpr uint8_t kIpv4Version = 4;
constexpr uint8_t kIpv4HeaderLength = 20;
constexpr uint8_t kUdpHeaderLength = 8;
constexpr uint8_t kIcmpTypeEchoReply = 0;
constexpr uint8_t kIcmpTypeEchoRequest = 8;
constexpr uint8_t kArpPayloadLength = 28;
constexpr uint8_t kArpCacheSize = 8;
constexpr uint16_t kNetstackMaxUdpPayload = ETHERNET_MAX_PAYLOAD_SIZE - 20 - 8;
constexpr uint32_t kPollBudget = 32;
constexpr uint64_t kArpWaitTicks = 25;
constexpr uint64_t kDhcpTimeoutTicks = 500;
constexpr uint64_t kDhcpRetryTicks = 100;
constexpr uint8_t kDhcpDiscoverLimit = 4;
constexpr uint8_t kArpRetryCount = 3;
constexpr uint16_t kPingIdentifier = 0x4C41;
constexpr uint16_t kDhcpClientPort = 68;
constexpr uint16_t kDhcpServerPort = 67;
constexpr uint8_t kDhcpBootRequest = 1;
constexpr uint8_t kDhcpBootReply = 2;
constexpr uint8_t kDhcpHardwareTypeEthernet = 1;
constexpr uint8_t kDhcpDiscover = 1;
constexpr uint8_t kDhcpOffer = 2;
constexpr uint8_t kDhcpRequest = 3;
constexpr uint8_t kDhcpAck = 5;
constexpr uint8_t kDhcpNak = 6;
constexpr uint8_t kDhcpOptionSubnetMask = 1;
constexpr uint8_t kDhcpOptionRouter = 3;
constexpr uint8_t kDhcpOptionDns = 6;
constexpr uint8_t kDhcpOptionRequestedIp = 50;
constexpr uint8_t kDhcpOptionLeaseTime = 51;
constexpr uint8_t kDhcpOptionMessageType = 53;
constexpr uint8_t kDhcpOptionServerIdentifier = 54;
constexpr uint8_t kDhcpOptionParameterRequestList = 55;
constexpr uint8_t kDhcpOptionClientIdentifier = 61;
constexpr uint8_t kDhcpOptionEnd = 255;
constexpr uint32_t kDhcpMagicCookie = 0x63825363u;
constexpr uint16_t kDhcpBootpHeaderLength = 236;
constexpr uint16_t kDhcpMinimumPayloadLength = kDhcpBootpHeaderLength + 4;
constexpr uint16_t kDhcpDefaultLeaseSeconds = 3600;
constexpr uint16_t kDnsServerPort = 53;
constexpr uint16_t kDnsSourcePortBase = 53000;
constexpr uint16_t kDnsHeaderLength = 12;
constexpr uint16_t kDnsMaxPacketLength = 512;
constexpr uint16_t kDnsTypeA = 1;
constexpr uint16_t kDnsTypeAaaa = 28;
constexpr uint16_t kDnsClassIn = 1;
constexpr uint64_t kDnsWaitTicks = 100;
constexpr uint64_t kPingWaitTicks = 100;
constexpr uint8_t kQemuDefaultAddress[4] = {10, 0, 2, 15};
constexpr uint8_t kQemuDefaultMask[4] = {255, 255, 255, 0};
constexpr uint8_t kQemuDefaultGateway[4] = {10, 0, 2, 2};
constexpr uint8_t kQemuDefaultDns[4] = {10, 0, 2, 3};

enum netstack_dhcp_phase : uint8_t {
    NETSTACK_DHCP_DISABLED = 0,
    NETSTACK_DHCP_DISCOVERING = 1,
    NETSTACK_DHCP_REQUESTING = 2,
    NETSTACK_DHCP_BOUND = 3,
    NETSTACK_DHCP_FAILED = 4
};

struct arp_cache_entry {
    bool valid;
    uint8_t ip[4];
    uint8_t mac[ETHERNET_ADDRESS_LENGTH];
    uint32_t age;
};

struct pending_ping_state {
    bool active;
    bool received_reply;
    uint8_t target_ip[4];
    uint16_t sequence;
};

struct pending_dns_state {
    bool active;
    bool received_reply;
    uint16_t transaction_id;
    uint16_t source_port;
    uint16_t query_type;
    uint8_t resolved_ip[16];
};

struct dhcp_runtime_state {
    bool enabled;
    bool bound;
    bool failed;
    uint8_t phase;
    uint32_t transaction_id;
    uint32_t attempts;
    uint32_t lease_time_seconds;
    uint8_t offered_ip[4];
    uint8_t server_ip[4];
};

struct netstack_state {
    bool initialized;
    bool ipv4_configured;
    bool using_dhcp;
    uint8_t local_mac[ETHERNET_ADDRESS_LENGTH];
    uint8_t local_ip[4];
    uint8_t subnet_mask[4];
    uint8_t gateway[4];
    uint8_t dns_server[4];
    arp_cache_entry arp_cache[kArpCacheSize];
    pending_ping_state pending_ping;
    pending_dns_state pending_dns;
    dhcp_runtime_state dhcp;
    uint32_t arp_requests_sent;
    uint32_t arp_replies_sent;
    uint32_t arp_packets_received;
    uint32_t ipv4_packets_received;
    uint32_t ipv4_packets_sent;
    uint32_t udp_packets_received;
    uint32_t udp_packets_sent;
    uint32_t dhcp_discovers_sent;
    uint32_t dhcp_requests_sent;
    uint32_t dhcp_offers_received;
    uint32_t dhcp_acks_received;
    uint32_t icmp_echo_requests_sent;
    uint32_t icmp_echo_replies_sent;
    uint32_t icmp_echo_requests_received;
    uint32_t icmp_echo_replies_received;
    uint32_t dropped_packets;
    uint32_t handled_packets;
    uint32_t arp_age_counter;
    uint16_t ipv4_identification;
};

static netstack_state g_state = {};

static void copy_bytes(uint8_t* destination, const uint8_t* source, uint16_t length) {
    if (length == 0) {
        return;
    }

    memcpy(destination, source, length);
}

static bool bytes_equal(const uint8_t* left, const uint8_t* right, uint16_t length) {
    for (uint16_t index = 0; index < length; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

static bool ipv4_equal(const uint8_t left[4], const uint8_t right[4]) {
    return bytes_equal(left, right, 4);
}

static bool mac_equal(const uint8_t left[ETHERNET_ADDRESS_LENGTH], const uint8_t right[ETHERNET_ADDRESS_LENGTH]) {
    return bytes_equal(left, right, ETHERNET_ADDRESS_LENGTH);
}

static bool ipv4_is_zero(const uint8_t ip[4]) {
    return ipv4_equal(ip, kZeroIpv4);
}

static bool ipv4_is_global_broadcast(const uint8_t ip[4]) {
    return ipv4_equal(ip, kIpv4Broadcast);
}

static void write_be16(uint8_t* destination, uint16_t value) {
    destination[0] = static_cast<uint8_t>(value >> 8);
    destination[1] = static_cast<uint8_t>(value & 0xFF);
}

static void write_be32(uint8_t* destination, uint32_t value) {
    destination[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    destination[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    destination[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    destination[3] = static_cast<uint8_t>(value & 0xFF);
}

static uint16_t read_be16(const uint8_t* source) {
    return static_cast<uint16_t>((source[0] << 8) | source[1]);
}

static uint32_t read_be32(const uint8_t* source) {
    return (static_cast<uint32_t>(source[0]) << 24) |
           (static_cast<uint32_t>(source[1]) << 16) |
           (static_cast<uint32_t>(source[2]) << 8) |
           static_cast<uint32_t>(source[3]);
}

static void clear_pending_ping() {
    memset(&g_state.pending_ping, 0, sizeof(g_state.pending_ping));
}

static void clear_pending_dns() {
    memset(&g_state.pending_dns, 0, sizeof(g_state.pending_dns));
}

static void clear_dhcp_runtime() {
    memset(&g_state.dhcp, 0, sizeof(g_state.dhcp));
    g_state.dhcp.phase = NETSTACK_DHCP_DISABLED;
}

static uint32_t checksum_accumulate(uint32_t sum, const uint8_t* data, uint16_t length) {
    uint16_t index = 0;

    while ((index + 1) < length) {
        sum += static_cast<uint16_t>((data[index] << 8) | data[index + 1]);
        index += 2;
    }

    if ((length & 1u) != 0) {
        sum += static_cast<uint16_t>(data[length - 1] << 8);
    }

    return sum;
}

static uint16_t checksum_finalize(uint32_t sum) {
    while ((sum >> 16) != 0) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }

    return static_cast<uint16_t>(~sum);
}

static uint16_t checksum16(const uint8_t* data, uint16_t length) {
    return checksum_finalize(checksum_accumulate(0, data, length));
}

static uint16_t ipv4_transport_checksum(
    const uint8_t source_ip[4],
    const uint8_t destination_ip[4],
    uint8_t protocol,
    const uint8_t* payload,
    uint16_t payload_length
) {
    uint32_t sum = 0;
    sum = checksum_accumulate(sum, source_ip, 4);
    sum = checksum_accumulate(sum, destination_ip, 4);

    uint8_t pseudo_header_tail[4];
    pseudo_header_tail[0] = 0;
    pseudo_header_tail[1] = protocol;
    pseudo_header_tail[2] = static_cast<uint8_t>(payload_length >> 8);
    pseudo_header_tail[3] = static_cast<uint8_t>(payload_length & 0xFF);
    sum = checksum_accumulate(sum, pseudo_header_tail, sizeof(pseudo_header_tail));
    sum = checksum_accumulate(sum, payload, payload_length);
    return checksum_finalize(sum);
}

static int arp_cache_find(const uint8_t ip[4]) {
    for (int index = 0; index < kArpCacheSize; ++index) {
        if (g_state.arp_cache[index].valid && ipv4_equal(g_state.arp_cache[index].ip, ip)) {
            return index;
        }
    }

    return -1;
}

static void arp_cache_store(const uint8_t ip[4], const uint8_t mac[ETHERNET_ADDRESS_LENGTH]) {
    if (ipv4_is_zero(ip)) {
        return;
    }

    int slot = arp_cache_find(ip);
    if (slot < 0) {
        slot = 0;
        for (int index = 0; index < kArpCacheSize; ++index) {
            if (!g_state.arp_cache[index].valid) {
                slot = index;
                break;
            }

            if (g_state.arp_cache[index].age < g_state.arp_cache[slot].age) {
                slot = index;
            }
        }
    }

    g_state.arp_cache[slot].valid = true;
    copy_bytes(g_state.arp_cache[slot].ip, ip, 4);
    copy_bytes(g_state.arp_cache[slot].mac, mac, ETHERNET_ADDRESS_LENGTH);
    g_state.arp_cache[slot].age = ++g_state.arp_age_counter;
}

static bool arp_cache_lookup(const uint8_t ip[4], uint8_t mac[ETHERNET_ADDRESS_LENGTH]) {
    const int slot = arp_cache_find(ip);
    if (slot < 0) {
        return false;
    }

    copy_bytes(mac, g_state.arp_cache[slot].mac, ETHERNET_ADDRESS_LENGTH);
    g_state.arp_cache[slot].age = ++g_state.arp_age_counter;
    return true;
}

static void set_ipv4_configuration(
    const uint8_t address[4],
    const uint8_t subnet_mask[4],
    const uint8_t gateway[4],
    const uint8_t dns_server[4],
    bool using_dhcp
) {
    copy_bytes(g_state.local_ip, address, 4);
    copy_bytes(g_state.subnet_mask, subnet_mask, 4);
    copy_bytes(g_state.gateway, gateway, 4);
    copy_bytes(g_state.dns_server, dns_server, 4);
    g_state.ipv4_configured = !ipv4_is_zero(address);
    g_state.using_dhcp = using_dhcp;
    memset(g_state.arp_cache, 0, sizeof(g_state.arp_cache));
    clear_pending_ping();
    clear_pending_dns();
}

static void clear_ipv4_state_internal() {
    set_ipv4_configuration(kZeroIpv4, kZeroIpv4, kZeroIpv4, kZeroIpv4, false);
    clear_dhcp_runtime();
}

static bool ipv4_is_local_broadcast(const uint8_t ip[4]) {
    if (!g_state.ipv4_configured) {
        return false;
    }

    uint8_t broadcast[4];
    for (int index = 0; index < 4; ++index) {
        broadcast[index] = static_cast<uint8_t>((g_state.local_ip[index] & g_state.subnet_mask[index]) |
                                                (~g_state.subnet_mask[index]));
    }

    return ipv4_equal(ip, broadcast);
}

static bool ipv4_is_same_subnet(const uint8_t ip[4]) {
    if (!g_state.ipv4_configured) {
        return false;
    }

    for (int index = 0; index < 4; ++index) {
        if ((ip[index] & g_state.subnet_mask[index]) != (g_state.local_ip[index] & g_state.subnet_mask[index])) {
            return false;
        }
    }

    return true;
}

static bool ipv4_choose_next_hop(const uint8_t destination[4], uint8_t next_hop[4]) {
    if (!g_state.ipv4_configured) {
        return false;
    }

    if (ipv4_is_global_broadcast(destination) || ipv4_is_local_broadcast(destination)) {
        copy_bytes(next_hop, destination, 4);
        return true;
    }

    if (ipv4_is_same_subnet(destination)) {
        copy_bytes(next_hop, destination, 4);
        return true;
    }

    if (ipv4_is_zero(g_state.gateway)) {
        return false;
    }

    copy_bytes(next_hop, g_state.gateway, 4);
    return true;
}

static void netstack_consume_frames();

static bool send_arp_packet(
    const uint8_t destination_mac[ETHERNET_ADDRESS_LENGTH],
    uint16_t operation,
    const uint8_t sender_mac[ETHERNET_ADDRESS_LENGTH],
    const uint8_t sender_ip[4],
    const uint8_t target_mac[ETHERNET_ADDRESS_LENGTH],
    const uint8_t target_ip[4]
) {
    uint8_t payload[kArpPayloadLength];
    memset(payload, 0, sizeof(payload));

    write_be16(&payload[0], kArpHardwareTypeEthernet);
    write_be16(&payload[2], kEtherTypeIpv4);
    payload[4] = ETHERNET_ADDRESS_LENGTH;
    payload[5] = 4;
    write_be16(&payload[6], operation);
    copy_bytes(&payload[8], sender_mac, ETHERNET_ADDRESS_LENGTH);
    copy_bytes(&payload[14], sender_ip, 4);
    copy_bytes(&payload[18], target_mac, ETHERNET_ADDRESS_LENGTH);
    copy_bytes(&payload[24], target_ip, 4);

    return ethernet_send(destination_mac, kEtherTypeArp, payload, sizeof(payload));
}

static bool send_arp_request(const uint8_t sender_ip[4], const uint8_t target_ip[4]) {
    uint8_t empty_target_mac[ETHERNET_ADDRESS_LENGTH] = {};
    if (!send_arp_packet(
            kBroadcastMac,
            kArpOperationRequest,
            g_state.local_mac,
            sender_ip,
            empty_target_mac,
            target_ip)) {
        return false;
    }

    ++g_state.arp_requests_sent;
    return true;
}

static bool send_arp_reply(
    const uint8_t destination_mac[ETHERNET_ADDRESS_LENGTH],
    const uint8_t destination_ip[4]
) {
    if (!send_arp_packet(
            destination_mac,
            kArpOperationReply,
            g_state.local_mac,
            g_state.local_ip,
            destination_mac,
            destination_ip)) {
        return false;
    }

    ++g_state.arp_replies_sent;
    return true;
}

static bool send_ipv4_frame_raw(
    const uint8_t source_ip[4],
    const uint8_t destination_mac[ETHERNET_ADDRESS_LENGTH],
    const uint8_t destination_ip[4],
    uint8_t protocol,
    const uint8_t* payload,
    uint16_t payload_length
) {
    if (payload_length > (ETHERNET_MAX_PAYLOAD_SIZE - kIpv4HeaderLength)) {
        return false;
    }

    uint8_t frame[kIpv4HeaderLength + ETHERNET_MAX_PAYLOAD_SIZE];
    memset(frame, 0, sizeof(frame));

    const uint16_t total_length = static_cast<uint16_t>(kIpv4HeaderLength + payload_length);
    frame[0] = static_cast<uint8_t>((kIpv4Version << 4) | (kIpv4HeaderLength / 4));
    frame[1] = 0x00;
    write_be16(&frame[2], total_length);

    const uint16_t identification = ++g_state.ipv4_identification;
    write_be16(&frame[4], identification);
    frame[6] = 0x40;
    frame[7] = 0x00;
    frame[8] = 64;
    frame[9] = protocol;
    copy_bytes(&frame[12], source_ip, 4);
    copy_bytes(&frame[16], destination_ip, 4);

    if (payload_length > 0 && payload != nullptr) {
        copy_bytes(&frame[kIpv4HeaderLength], payload, payload_length);
    }

    write_be16(&frame[10], checksum16(frame, kIpv4HeaderLength));
    if (!ethernet_send(destination_mac, kEtherTypeIpv4, frame, total_length)) {
        return false;
    }

    ++g_state.ipv4_packets_sent;
    return true;
}

static bool send_udp_packet_raw(
    const uint8_t source_ip[4],
    const uint8_t destination_mac[ETHERNET_ADDRESS_LENGTH],
    const uint8_t destination_ip[4],
    uint16_t source_port,
    uint16_t destination_port,
    const uint8_t* payload,
    uint16_t payload_length
) {
    if (payload_length > kNetstackMaxUdpPayload) {
        return false;
    }

    const uint16_t udp_length = static_cast<uint16_t>(kUdpHeaderLength + payload_length);
    uint8_t datagram[kUdpHeaderLength + kNetstackMaxUdpPayload];
    memset(datagram, 0, sizeof(datagram));

    write_be16(&datagram[0], source_port);
    write_be16(&datagram[2], destination_port);
    write_be16(&datagram[4], udp_length);
    write_be16(&datagram[6], 0);
    if (payload_length > 0 && payload != nullptr) {
        copy_bytes(&datagram[kUdpHeaderLength], payload, payload_length);
    }

    uint16_t udp_checksum = ipv4_transport_checksum(source_ip, destination_ip, kIpv4ProtocolUdp, datagram, udp_length);
    if (udp_checksum == 0) {
        udp_checksum = 0xFFFFu;
    }
    write_be16(&datagram[6], udp_checksum);

    if (!send_ipv4_frame_raw(source_ip, destination_mac, destination_ip, kIpv4ProtocolUdp, datagram, udp_length)) {
        return false;
    }

    ++g_state.udp_packets_sent;
    return true;
}

static bool send_icmp_echo(
    const uint8_t source_ip[4],
    const uint8_t destination_mac[ETHERNET_ADDRESS_LENGTH],
    const uint8_t destination_ip[4],
    uint8_t type,
    uint16_t sequence,
    const uint8_t* body,
    uint16_t body_length
) {
    uint8_t packet[ETHERNET_MAX_PAYLOAD_SIZE - kIpv4HeaderLength];
    const uint16_t icmp_length = static_cast<uint16_t>(8 + body_length);
    if (icmp_length > sizeof(packet)) {
        return false;
    }

    memset(packet, 0, sizeof(packet));
    packet[0] = type;
    packet[1] = 0;
    write_be16(&packet[4], kPingIdentifier);
    write_be16(&packet[6], sequence);
    if (body_length > 0 && body != nullptr) {
        copy_bytes(&packet[8], body, body_length);
    }

    write_be16(&packet[2], checksum16(packet, icmp_length));
    return send_ipv4_frame_raw(source_ip, destination_mac, destination_ip, kIpv4ProtocolIcmp, packet, icmp_length);
}

static void dhcp_enable_runtime() {
    clear_dhcp_runtime();
    g_state.dhcp.enabled = true;
    g_state.dhcp.phase = NETSTACK_DHCP_DISCOVERING;
    g_state.dhcp.transaction_id =
        0x4C410000u |
        (static_cast<uint32_t>(g_state.local_mac[4]) << 8) |
        static_cast<uint32_t>(g_state.local_mac[5]);
}

static bool send_dhcp_payload(uint8_t message_type) {
    static const uint8_t kParameterRequestList[] = {
        kDhcpOptionSubnetMask,
        kDhcpOptionRouter,
        kDhcpOptionDns,
        kDhcpOptionLeaseTime,
        kDhcpOptionServerIdentifier
    };

    uint8_t payload[280];
    memset(payload, 0, sizeof(payload));

    payload[0] = kDhcpBootRequest;
    payload[1] = kDhcpHardwareTypeEthernet;
    payload[2] = ETHERNET_ADDRESS_LENGTH;
    write_be32(&payload[4], g_state.dhcp.transaction_id);
    write_be16(&payload[10], 0x8000);
    copy_bytes(&payload[28], g_state.local_mac, ETHERNET_ADDRESS_LENGTH);
    write_be32(&payload[kDhcpBootpHeaderLength], kDhcpMagicCookie);

    uint8_t* option = &payload[kDhcpBootpHeaderLength + 4];
    option[0] = kDhcpOptionMessageType;
    option[1] = 1;
    option[2] = message_type;
    option += 3;

    option[0] = kDhcpOptionClientIdentifier;
    option[1] = 1 + ETHERNET_ADDRESS_LENGTH;
    option[2] = kDhcpHardwareTypeEthernet;
    copy_bytes(&option[3], g_state.local_mac, ETHERNET_ADDRESS_LENGTH);
    option += 3 + ETHERNET_ADDRESS_LENGTH;

    if (message_type == kDhcpRequest) {
        option[0] = kDhcpOptionRequestedIp;
        option[1] = 4;
        copy_bytes(&option[2], g_state.dhcp.offered_ip, 4);
        option += 6;

        option[0] = kDhcpOptionServerIdentifier;
        option[1] = 4;
        copy_bytes(&option[2], g_state.dhcp.server_ip, 4);
        option += 6;
    }

    option[0] = kDhcpOptionParameterRequestList;
    option[1] = sizeof(kParameterRequestList);
    copy_bytes(&option[2], kParameterRequestList, sizeof(kParameterRequestList));
    option += 2 + sizeof(kParameterRequestList);
    *option++ = kDhcpOptionEnd;

    return send_udp_packet_raw(
        kZeroIpv4,
        kBroadcastMac,
        kIpv4Broadcast,
        kDhcpClientPort,
        kDhcpServerPort,
        payload,
        static_cast<uint16_t>(option - payload)
    );
}

static bool send_dhcp_discover() {
    if (!send_dhcp_payload(kDhcpDiscover)) {
        return false;
    }

    ++g_state.dhcp.attempts;
    ++g_state.dhcp_discovers_sent;
    g_state.dhcp.phase = NETSTACK_DHCP_DISCOVERING;
    return true;
}

static bool send_dhcp_request() {
    if (!send_dhcp_payload(kDhcpRequest)) {
        return false;
    }

    ++g_state.dhcp.attempts;
    ++g_state.dhcp_requests_sent;
    g_state.dhcp.phase = NETSTACK_DHCP_REQUESTING;
    return true;
}

static bool handle_dhcp_packet(const uint8_t* payload, uint16_t payload_length) {
    if (!g_state.dhcp.enabled || payload == nullptr || payload_length < kDhcpMinimumPayloadLength) {
        return false;
    }

    if (payload[0] != kDhcpBootReply ||
        payload[1] != kDhcpHardwareTypeEthernet ||
        payload[2] != ETHERNET_ADDRESS_LENGTH ||
        read_be32(&payload[4]) != g_state.dhcp.transaction_id ||
        !mac_equal(&payload[28], g_state.local_mac) ||
        read_be32(&payload[kDhcpBootpHeaderLength]) != kDhcpMagicCookie) {
        return false;
    }

    uint8_t yiaddr[4];
    uint8_t server_ip[4] = {};
    uint8_t subnet_mask[4] = {};
    uint8_t gateway[4] = {};
    uint8_t dns_server[4] = {};
    uint32_t lease_time_seconds = kDhcpDefaultLeaseSeconds;
    uint8_t message_type = 0;
    copy_bytes(yiaddr, &payload[16], 4);

    uint16_t offset = static_cast<uint16_t>(kDhcpBootpHeaderLength + 4);
    while (offset < payload_length) {
        const uint8_t option = payload[offset++];
        if (option == 0) {
            continue;
        }
        if (option == kDhcpOptionEnd) {
            break;
        }
        if (offset >= payload_length) {
            return false;
        }

        const uint8_t option_length = payload[offset++];
        if (static_cast<uint16_t>(offset + option_length) > payload_length) {
            return false;
        }

        const uint8_t* option_value = &payload[offset];
        if (option == kDhcpOptionMessageType && option_length == 1) {
            message_type = option_value[0];
        } else if (option_length >= 4) {
            if (option == kDhcpOptionServerIdentifier) {
                copy_bytes(server_ip, option_value, 4);
            } else if (option == kDhcpOptionSubnetMask) {
                copy_bytes(subnet_mask, option_value, 4);
            } else if (option == kDhcpOptionRouter) {
                copy_bytes(gateway, option_value, 4);
            } else if (option == kDhcpOptionDns) {
                copy_bytes(dns_server, option_value, 4);
            } else if (option == kDhcpOptionLeaseTime) {
                lease_time_seconds = read_be32(option_value);
            }
        }

        offset = static_cast<uint16_t>(offset + option_length);
    }

    if (message_type == kDhcpOffer) {
        ++g_state.dhcp_offers_received;
        copy_bytes(g_state.dhcp.offered_ip, yiaddr, 4);
        if (!ipv4_is_zero(server_ip)) {
            copy_bytes(g_state.dhcp.server_ip, server_ip, 4);
        }
        if (!send_dhcp_request()) {
            g_state.dhcp.failed = true;
            g_state.dhcp.phase = NETSTACK_DHCP_FAILED;
        }
        return true;
    }

    if (message_type == kDhcpAck) {
        ++g_state.dhcp_acks_received;
        const uint8_t* mask = ipv4_is_zero(subnet_mask) ? kQemuDefaultMask : subnet_mask;
        const uint8_t* route = ipv4_is_zero(gateway) ? kZeroIpv4 : gateway;
        const uint8_t* dns = ipv4_is_zero(dns_server) ? kZeroIpv4 : dns_server;
        set_ipv4_configuration(yiaddr, mask, route, dns, true);
        g_state.dhcp.bound = true;
        g_state.dhcp.failed = false;
        g_state.dhcp.phase = NETSTACK_DHCP_BOUND;
        g_state.dhcp.lease_time_seconds = lease_time_seconds;
        if (!ipv4_is_zero(server_ip)) {
            copy_bytes(g_state.dhcp.server_ip, server_ip, 4);
        }
        return true;
    }

    if (message_type == kDhcpNak) {
        g_state.dhcp.bound = false;
        g_state.dhcp.failed = true;
        g_state.dhcp.phase = NETSTACK_DHCP_FAILED;
        return true;
    }

    return false;
}

static bool dns_skip_name(const uint8_t* packet, uint16_t packet_length, uint16_t* offset) {
    if (packet == nullptr || offset == nullptr || *offset >= packet_length) {
        return false;
    }

    uint16_t cursor = *offset;
    for (uint8_t parts = 0; parts < 64; ++parts) {
        if (cursor >= packet_length) {
            return false;
        }

        const uint8_t label_length = packet[cursor++];
        if (label_length == 0) {
            *offset = cursor;
            return true;
        }

        if ((label_length & 0xC0u) == 0xC0u) {
            if (cursor >= packet_length) {
                return false;
            }
            *offset = static_cast<uint16_t>(cursor + 1);
            return true;
        }

        if ((label_length & 0xC0u) != 0) {
            return false;
        }

        if (static_cast<uint16_t>(cursor + label_length) > packet_length) {
            return false;
        }
        cursor = static_cast<uint16_t>(cursor + label_length);
    }

    return false;
}

static bool dns_build_query(const char* hostname, uint16_t transaction_id,
                            uint16_t query_type, uint8_t* packet,
                            uint16_t* packet_length) {
    if (hostname == nullptr || hostname[0] == '\0' || packet == nullptr || packet_length == nullptr) {
        return false;
    }

    memset(packet, 0, kDnsMaxPacketLength);
    write_be16(&packet[0], transaction_id);
    write_be16(&packet[2], 0x0100); // recursion desired
    write_be16(&packet[4], 1);      // one question

    uint16_t out = kDnsHeaderLength;
    const char* label = hostname;
    while (*label != '\0') {
        if (*label == '.') {
            return false;
        }

        const char* cursor = label;
        uint8_t label_length = 0;
        while (*cursor != '\0' && *cursor != '.') {
            if ((unsigned char)*cursor <= ' ' || *cursor == '/' || *cursor == ':' || *cursor == '[' || *cursor == ']') {
                return false;
            }
            if (label_length >= 63) {
                return false;
            }
            ++label_length;
            ++cursor;
        }

        if (out + 1u + label_length + 5u > kDnsMaxPacketLength) {
            return false;
        }

        packet[out++] = label_length;
        for (uint8_t i = 0; i < label_length; ++i) {
            packet[out++] = static_cast<uint8_t>(label[i]);
        }

        if (*cursor == '.') {
            label = cursor + 1;
        } else {
            label = cursor;
        }
    }

    packet[out++] = 0;
    write_be16(&packet[out], query_type);
    out = static_cast<uint16_t>(out + 2);
    write_be16(&packet[out], kDnsClassIn);
    out = static_cast<uint16_t>(out + 2);
    *packet_length = out;
    return true;
}

static bool handle_dns_packet(uint16_t source_port, uint16_t destination_port, const uint8_t* payload, uint16_t payload_length) {
    if (!g_state.pending_dns.active ||
        source_port != kDnsServerPort ||
        destination_port != g_state.pending_dns.source_port ||
        payload == nullptr ||
        payload_length < kDnsHeaderLength) {
        return false;
    }

    if (read_be16(&payload[0]) != g_state.pending_dns.transaction_id) {
        return false;
    }

    const uint16_t flags = read_be16(&payload[2]);
    if ((flags & 0x8000u) == 0) {
        return false;
    }

    const uint16_t question_count = read_be16(&payload[4]);
    const uint16_t answer_count = read_be16(&payload[6]);
    uint16_t offset = kDnsHeaderLength;

    for (uint16_t q = 0; q < question_count; ++q) {
        if (!dns_skip_name(payload, payload_length, &offset)) {
            g_state.pending_dns.active = false;
            return true;
        }
        if (static_cast<uint16_t>(offset + 4) > payload_length) {
            g_state.pending_dns.active = false;
            return true;
        }
        offset = static_cast<uint16_t>(offset + 4);
    }

    for (uint16_t a = 0; a < answer_count; ++a) {
        if (!dns_skip_name(payload, payload_length, &offset)) {
            g_state.pending_dns.active = false;
            return true;
        }
        if (static_cast<uint16_t>(offset + 10) > payload_length) {
            g_state.pending_dns.active = false;
            return true;
        }

        const uint16_t type = read_be16(&payload[offset]);
        const uint16_t klass = read_be16(&payload[offset + 2]);
        const uint16_t data_length = read_be16(&payload[offset + 8]);
        offset = static_cast<uint16_t>(offset + 10);
        if (static_cast<uint16_t>(offset + data_length) > payload_length) {
            g_state.pending_dns.active = false;
            return true;
        }

        const uint16_t expected_length =
            g_state.pending_dns.query_type == kDnsTypeA ? 4 : 16;
        if (type == g_state.pending_dns.query_type && klass == kDnsClassIn &&
            data_length == expected_length) {
            copy_bytes(g_state.pending_dns.resolved_ip, &payload[offset],
                       expected_length);
            g_state.pending_dns.received_reply = true;
            g_state.pending_dns.active = false;
            return true;
        }

        offset = static_cast<uint16_t>(offset + data_length);
    }

    g_state.pending_dns.active = false;
    return true;
}

static bool resolve_mac_with_arp(const uint8_t source_ip[4], const uint8_t target_ip[4], uint8_t mac[ETHERNET_ADDRESS_LENGTH]) {
    if (arp_cache_lookup(target_ip, mac)) {
        return true;
    }

    for (uint8_t attempt = 0; attempt < kArpRetryCount; ++attempt) {
        if (!send_arp_request(source_ip, target_ip)) {
            continue;
        }

        const uint64_t deadline = scheduler_tick_count() + kArpWaitTicks;
        while (scheduler_tick_count() < deadline) {
            netstack_consume_frames();
            if (arp_cache_lookup(target_ip, mac)) {
                return true;
            }
            (void)scheduler_sleep(1);
        }
    }

    return false;
}

static void handle_arp_packet(const ethernet_packet& packet) {
    if (packet.payload_length < kArpPayloadLength) {
        ++g_state.dropped_packets;
        return;
    }

    ++g_state.arp_packets_received;

    const uint16_t hardware_type = read_be16(&packet.payload[0]);
    const uint16_t protocol_type = read_be16(&packet.payload[2]);
    const uint16_t operation = read_be16(&packet.payload[6]);
    const uint8_t hardware_length = packet.payload[4];
    const uint8_t protocol_length = packet.payload[5];

    if (hardware_type != kArpHardwareTypeEthernet ||
        protocol_type != kEtherTypeIpv4 ||
        hardware_length != ETHERNET_ADDRESS_LENGTH ||
        protocol_length != 4) {
        ++g_state.dropped_packets;
        return;
    }

    const uint8_t* sender_mac = &packet.payload[8];
    const uint8_t* sender_ip = &packet.payload[14];
    const uint8_t* target_ip = &packet.payload[24];

    arp_cache_store(sender_ip, sender_mac);

    if (g_state.ipv4_configured &&
        operation == kArpOperationRequest &&
        ipv4_equal(target_ip, g_state.local_ip)) {
        if (send_arp_reply(sender_mac, sender_ip)) {
            ++g_state.handled_packets;
        }
        return;
    }

    if (operation == kArpOperationReply) {
        ++g_state.handled_packets;
    }
}

static void handle_icmp_echo_request(const uint8_t source_ip[4], const ethernet_packet& packet, uint16_t payload_offset, uint16_t payload_length) {
    if (payload_length < 8 || !g_state.ipv4_configured) {
        ++g_state.dropped_packets;
        return;
    }

    const uint16_t sequence = read_be16(&packet.payload[payload_offset + 6]);
    if (send_icmp_echo(g_state.local_ip, packet.source, source_ip, kIcmpTypeEchoReply, sequence, &packet.payload[payload_offset + 8], static_cast<uint16_t>(payload_length - 8))) {
        ++g_state.icmp_echo_replies_sent;
        ++g_state.handled_packets;
    }
}

static void handle_icmp_echo_reply(const uint8_t source_ip[4], const uint8_t* payload, uint16_t payload_length) {
    if (payload_length < 8 || !g_state.pending_ping.active) {
        return;
    }

    const uint16_t identifier = read_be16(&payload[4]);
    const uint16_t sequence = read_be16(&payload[6]);
    if (identifier != kPingIdentifier ||
        sequence != g_state.pending_ping.sequence ||
        !ipv4_equal(source_ip, g_state.pending_ping.target_ip)) {
        return;
    }

    g_state.pending_ping.received_reply = true;
    ++g_state.handled_packets;
}

static void handle_udp_packet(
    const uint8_t source_ip[4],
    const uint8_t destination_ip[4],
    const uint8_t* payload,
    uint16_t payload_length
) {
    if (payload_length < kUdpHeaderLength) {
        ++g_state.dropped_packets;
        return;
    }

    ++g_state.udp_packets_received;

    const uint16_t source_port = read_be16(&payload[0]);
    const uint16_t destination_port = read_be16(&payload[2]);
    const uint16_t udp_length = read_be16(&payload[4]);
    const uint16_t udp_checksum = read_be16(&payload[6]);
    if (udp_length < kUdpHeaderLength || udp_length > payload_length) {
        ++g_state.dropped_packets;
        return;
    }

    if (udp_checksum != 0 && ipv4_transport_checksum(source_ip, destination_ip, kIpv4ProtocolUdp, payload, udp_length) != 0) {
        ++g_state.dropped_packets;
        return;
    }

    const uint8_t* udp_payload = &payload[kUdpHeaderLength];
    const uint16_t udp_payload_length = static_cast<uint16_t>(udp_length - kUdpHeaderLength);

    if (source_port == kDhcpServerPort && destination_port == kDhcpClientPort && handle_dhcp_packet(udp_payload, udp_payload_length)) {
        ++g_state.handled_packets;
        return;
    }

    if (handle_dns_packet(source_port, destination_port, udp_payload, udp_payload_length)) {
        ++g_state.handled_packets;
        return;
    }

    if (!g_state.ipv4_configured) {
        return;
    }

    if (!ipv4_equal(destination_ip, g_state.local_ip) &&
        !ipv4_is_global_broadcast(destination_ip) &&
        !ipv4_is_local_broadcast(destination_ip)) {
        return;
    }

    ++g_state.handled_packets;
}

static void handle_ipv4_packet(const ethernet_packet& packet) {
    if (packet.payload_length < kIpv4HeaderLength) {
        ++g_state.dropped_packets;
        return;
    }

    ++g_state.ipv4_packets_received;

    const uint8_t version_and_header_length = packet.payload[0];
    const uint8_t version = static_cast<uint8_t>(version_and_header_length >> 4);
    const uint16_t header_length = static_cast<uint16_t>((version_and_header_length & 0x0Fu) * 4u);
    if (version != kIpv4Version || header_length < kIpv4HeaderLength || header_length > packet.payload_length) {
        ++g_state.dropped_packets;
        return;
    }

    const uint16_t total_length = read_be16(&packet.payload[2]);
    if (total_length < header_length || total_length > packet.payload_length) {
        ++g_state.dropped_packets;
        return;
    }

    if (checksum16(packet.payload, header_length) != 0) {
        ++g_state.dropped_packets;
        return;
    }

    const uint8_t* source_ip = &packet.payload[12];
    const uint8_t* destination_ip = &packet.payload[16];
    const uint8_t protocol = packet.payload[9];
    const uint16_t ip_payload_length = static_cast<uint16_t>(total_length - header_length);
    const uint8_t* ip_payload = &packet.payload[header_length];

    arp_cache_store(source_ip, packet.source);

    if (protocol == kIpv4ProtocolUdp) {
        handle_udp_packet(source_ip, destination_ip, ip_payload, ip_payload_length);
        return;
    }

    if (protocol == kIpv4ProtocolTcp) {
        if (!g_state.ipv4_configured || !ipv4_equal(destination_ip, g_state.local_ip)) {
            return;
        }
        tcp_handle_packet(&packet);
        ++g_state.handled_packets;
        return;
    }

    if (!g_state.ipv4_configured) {
        return;
    }

    if (!ipv4_equal(destination_ip, g_state.local_ip) &&
        !ipv4_is_local_broadcast(destination_ip) &&
        !ipv4_is_global_broadcast(destination_ip)) {
        return;
    }

    if (protocol != kIpv4ProtocolIcmp) {
        return;
    }

    if (ip_payload_length < 8) {
        ++g_state.dropped_packets;
        return;
    }

    if (checksum16(ip_payload, ip_payload_length) != 0) {
        ++g_state.dropped_packets;
        return;
    }

    if (ip_payload[0] == kIcmpTypeEchoRequest) {
        ++g_state.icmp_echo_requests_received;
        handle_icmp_echo_request(source_ip, packet, header_length, ip_payload_length);
        return;
    }

    if (ip_payload[0] == kIcmpTypeEchoReply) {
        ++g_state.icmp_echo_replies_received;
        handle_icmp_echo_reply(source_ip, ip_payload, ip_payload_length);
    }
}

static void netstack_consume_frames() {
    if (!g_state.initialized) {
        return;
    }

    uint32_t processed = 0;
    ethernet_packet packet = {};
    while (processed < kPollBudget && ethernet_receive(&packet)) {
        if (packet.ether_type == kEtherTypeArp) {
            handle_arp_packet(packet);
        } else if (packet.ether_type == kEtherTypeIpv4) {
            handle_ipv4_packet(packet);
        } else if (packet.ether_type == 0x86DD) {
            netstack_ipv6_handle_packet(&packet);
        }

        ++processed;
    }
    netstack_ipv6_poll();
}

static bool run_dhcp_handshake() {
    dhcp_enable_runtime();
    clear_pending_ping();

    const uint64_t started = scheduler_tick_count();
    uint64_t next_discover = started;
    uint8_t discover_count = 0;
    while (scheduler_tick_count() - started < kDhcpTimeoutTicks) {
        netstack_consume_frames();
        if (g_state.dhcp.bound) {
            return true;
        }
        if (g_state.dhcp.failed) {
            return false;
        }

        const uint64_t now = scheduler_tick_count();
        if (discover_count < kDhcpDiscoverLimit && now >= next_discover) {
            if (send_dhcp_discover()) ++discover_count;
            next_discover = now + kDhcpRetryTicks;
        }
    }

    g_state.dhcp.failed = true;
    g_state.dhcp.phase = NETSTACK_DHCP_FAILED;
    return false;
}

static bool ping_ipv4_target(const uint8_t target_ip[4]) {
    if (!g_state.ipv4_configured) {
        return false;
    }

    uint8_t next_hop[4];
    if (!ipv4_choose_next_hop(target_ip, next_hop)) {
        return false;
    }

    uint8_t destination_mac[ETHERNET_ADDRESS_LENGTH];
    if (ipv4_is_global_broadcast(target_ip) || ipv4_is_local_broadcast(target_ip)) {
        copy_bytes(destination_mac, kBroadcastMac, ETHERNET_ADDRESS_LENGTH);
    } else if (!resolve_mac_with_arp(g_state.local_ip, next_hop, destination_mac)) {
        return false;
    }

    static const uint8_t kPingBody[] = {
        'L', 'a', 'w', 'A', 'I', ' ', 'p', 'i', 'n', 'g'
    };

    g_state.pending_ping.active = true;
    g_state.pending_ping.received_reply = false;
    copy_bytes(g_state.pending_ping.target_ip, target_ip, 4);
    ++g_state.pending_ping.sequence;

    if (!send_icmp_echo(g_state.local_ip, destination_mac, target_ip, kIcmpTypeEchoRequest, g_state.pending_ping.sequence, kPingBody, sizeof(kPingBody))) {
        clear_pending_ping();
        return false;
    }

    ++g_state.icmp_echo_requests_sent;

    const uint64_t deadline = scheduler_tick_count() + kPingWaitTicks;
    while (scheduler_tick_count() < deadline) {
        netstack_consume_frames();
        if (g_state.pending_ping.received_reply) {
            clear_pending_ping();
            return true;
        }
        (void)scheduler_sleep(1);
    }

    clear_pending_ping();
    return false;
}

} // namespace

bool netstack_init() {
    if (g_state.initialized) {
        return true;
    }

    if (!ethernet_is_ready()) {
        uprint("[net] ethernet not ready.\n");
        return false;
    }

    ethernet_info info = {};
    ethernet_get_info(&info);
    memset(&g_state, 0, sizeof(g_state));
    copy_bytes(g_state.local_mac, info.mac, ETHERNET_ADDRESS_LENGTH);
    g_state.initialized = true;
    clear_ipv4_state_internal();
    netstack_ipv6_init(g_state.local_mac);

    uprint("[net] DHCP probe...\n");
    if (run_dhcp_handshake()) {
        uprint("[net] DHCP ");
        uprint_ip(g_state.local_ip);
        uprint(".\n");
        // If DHCP succeeds, make sure the DNS server is not 0.0.0.0.
        if (ipv4_is_zero(g_state.dns_server)) {
            uprint("[net] WARN: DHCP did not provide DNS. Using 8.8.8.8\n");
            const uint8_t fallback_dns[4] = {8, 8, 8, 8};
            copy_bytes(g_state.dns_server, fallback_dns, 4);
        }
        return true;
    }

    uprint("[net] DHCP unavailable; IPv4 remains unconfigured.\n");
    set_ipv4_configuration(kZeroIpv4, kZeroIpv4, kZeroIpv4, kZeroIpv4, false);
    return true;
}

void netstack_poll() {
    netstack_consume_frames();
}

void netstack_get_info(netstack_info* out_info) {
    if (out_info == nullptr) {
        return;
    }

    memset(out_info, 0, sizeof(netstack_info));
    out_info->initialized = g_state.initialized;
    out_info->ipv4_configured = g_state.ipv4_configured;
    out_info->using_dhcp = g_state.using_dhcp;
    out_info->dhcp_phase = g_state.dhcp.phase;
    copy_bytes(out_info->mac, g_state.local_mac, ETHERNET_ADDRESS_LENGTH);
    copy_bytes(out_info->ip, g_state.local_ip, 4);
    copy_bytes(out_info->subnet_mask, g_state.subnet_mask, 4);
    copy_bytes(out_info->gateway, g_state.gateway, 4);
    copy_bytes(out_info->dns_server, g_state.dns_server, 4);
    out_info->arp_packets_received = g_state.arp_packets_received;
    out_info->ipv4_packets_received = g_state.ipv4_packets_received;
    out_info->ipv4_packets_sent = g_state.ipv4_packets_sent;
    out_info->icmp_echo_requests_sent = g_state.icmp_echo_requests_sent;
    out_info->icmp_echo_replies_received = g_state.icmp_echo_replies_received;
}

bool netstack_ping(const uint8_t target_ip[4]) {
    if (target_ip == nullptr) {
        return false;
    }

    return ping_ipv4_target(target_ip);
}

bool netstack_resolve_ipv4_mac(const uint8_t target_ip[4], uint8_t out_mac[6]) {
    if (!target_ip || !out_mac || !g_state.initialized || !g_state.ipv4_configured) {
        return false;
    }

    uint8_t next_hop[4];
    if (!ipv4_choose_next_hop(target_ip, next_hop)) return false;
    return resolve_mac_with_arp(g_state.local_ip, next_hop, out_mac);
}

bool netstack_parse_ipv4(const char* text, uint8_t out_ip[4]) {
    if (text == nullptr || out_ip == nullptr) {
        return false;
    }

    uint16_t value = 0;
    uint8_t octet_index = 0;
    bool have_digit = false;

    for (const char* cursor = text; ; ++cursor) {
        const char character = *cursor;

        if (character >= '0' && character <= '9') {
            value = static_cast<uint16_t>(value * 10 + (character - '0'));
            have_digit = true;
            if (value > 255) {
                return false;
            }
            continue;
        }

        if (character == '.' || character == '\0') {
            if (!have_digit || octet_index >= 4) {
                return false;
            }
            out_ip[octet_index++] = static_cast<uint8_t>(value);
            value = 0;
            have_digit = false;
            if (character == '\0') {
                break;
            }
            continue;
        }

        return false;
    }

    return octet_index == 4;
}

static bool netstack_dns_resolve_type(const char* hostname, uint16_t query_type,
                                      uint8_t* out_ip, uint16_t out_length) {
    if (hostname == nullptr || out_ip == nullptr ||
        (query_type == kDnsTypeA && out_length != 4) ||
        (query_type == kDnsTypeAaaa && out_length != 16)) {
        return false;
    }

    if (query_type == kDnsTypeA && netstack_parse_ipv4(hostname, out_ip)) {
        return true;
    }

    if (!g_state.initialized || !g_state.ipv4_configured || ipv4_is_zero(g_state.dns_server)) {
        return false;
    }

    uint8_t next_hop[4];
    if (!ipv4_choose_next_hop(g_state.dns_server, next_hop)) return false;

    uint8_t dns_mac[ETHERNET_ADDRESS_LENGTH];
    if (!resolve_mac_with_arp(g_state.local_ip, next_hop, dns_mac)) return false;

    const uint16_t transaction_id = static_cast<uint16_t>(0x4B00u ^ (++g_state.ipv4_identification));
    const uint16_t source_port = static_cast<uint16_t>(kDnsSourcePortBase + (transaction_id & 0x03FFu));
    uint8_t query[kDnsMaxPacketLength];
    uint16_t query_length = 0;
    if (!dns_build_query(hostname, transaction_id, query_type, query,
                         &query_length)) return false;

    for (uint8_t attempt = 0; attempt < 2; ++attempt) {
        g_state.pending_dns.active = true;
        g_state.pending_dns.received_reply = false;
        g_state.pending_dns.transaction_id = transaction_id;
        g_state.pending_dns.source_port = source_port;
        g_state.pending_dns.query_type = query_type;
        memset(g_state.pending_dns.resolved_ip, 0, sizeof(g_state.pending_dns.resolved_ip));

        if (!send_udp_packet_raw(
                g_state.local_ip,
                dns_mac,
                g_state.dns_server,
                source_port,
                kDnsServerPort,
                query,
                query_length)) {
            clear_pending_dns();
            continue;
        }

        const uint64_t deadline = scheduler_tick_count() + kDnsWaitTicks;
        while (scheduler_tick_count() < deadline) {
            netstack_consume_frames();
            if (g_state.pending_dns.received_reply) {
                copy_bytes(out_ip, g_state.pending_dns.resolved_ip, out_length);
                clear_pending_dns();
                return true;
            }
            if (!g_state.pending_dns.active) {
                break;
            }
            (void)scheduler_sleep(1);
        }
    }

    clear_pending_dns();
    return false;
}

bool netstack_dns_resolve(const char* hostname, uint8_t out_ip[4]) {
    return netstack_dns_resolve_type(hostname, kDnsTypeA, out_ip, 4);
}

bool netstack_dns_resolve_ipv6(const char* hostname, uint8_t out_ip[16]) {
    return netstack_dns_resolve_type(hostname, kDnsTypeAaaa, out_ip, 16);
}

void netstack_debug_print_info() {
    if (!g_state.initialized) {
        uprint("[net] stack not initialized.\n");
        return;
    }

    uprint("[net] mac=");
    eth_dbg_print_mac(g_state.local_mac);
    uprint(" ip=");
    if (g_state.ipv4_configured) {
        eth_dbg_print_ipv4(g_state.local_ip);
        uprint(" gw=");
        eth_dbg_print_ipv4(g_state.gateway);
    } else {
        uprint("none");
    }

    uprint(" dhcp=");
    eth_dbg_print_dec(g_state.dhcp.phase);
    uprint(" arp=");
    eth_dbg_print_dec(g_state.arp_packets_received);
    uprint(" rx=");
    eth_dbg_print_dec(g_state.ipv4_packets_received);
    uprint(" tx=");
    eth_dbg_print_dec(g_state.ipv4_packets_sent);
    uprint(".\n");
}

void netstack_debug_use_qemu_user_defaults() {
    if (!g_state.initialized) {
        uprint("[net] stack not initialized.\n");
        return;
    }

    set_ipv4_configuration(
        kQemuDefaultAddress,
        kQemuDefaultMask,
        kQemuDefaultGateway,
        kQemuDefaultDns,
        false
    );
    clear_dhcp_runtime();

    uprint("[net] static ");
    uprint_ip(g_state.local_ip);
    uprint(".\n");
}

void netstack_debug_resolve_gateway() {
    if (!g_state.initialized || !g_state.ipv4_configured) {
        uprint("[net] set IPv4 first.\n");
        return;
    }

    uint8_t gateway_mac[ETHERNET_ADDRESS_LENGTH];
    if (!resolve_mac_with_arp(g_state.local_ip, g_state.gateway, gateway_mac)) {
        uprint("[net] ARP failed.\n");
        return;
    }

    uprint("[net] gw ");
    eth_dbg_print_mac(gateway_mac);
    uprint(".\n");
}

void netstack_debug_ping_gateway() {
    if (!g_state.initialized || !g_state.ipv4_configured) {
        uprint("[net] set IPv4 first.\n");
        return;
    }

    uprint("[net] ping ");
    uprint_ip(g_state.gateway);
    uprint(" ... ");

    if (ping_ipv4_target(g_state.gateway)) {
        uprint("reply.\n");
        return;
    }

    uprint("timeout.\n");
}
