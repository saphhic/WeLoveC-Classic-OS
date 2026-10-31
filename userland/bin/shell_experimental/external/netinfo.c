// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Displays Ethernet status and network counters.

#include "cmd_common.h"
#include "user/network.h"

static void print_ipv4(const uint8_t address[4]) {
    for (uint32_t index = 0; index < 4; ++index) {
        if (index) cmd_print(".", CMD_WHITE);
        cmd_print_number(address[index], CMD_YELLOW);
    }
}

static void print_ipv6(const uint8_t address[16]) {
    for (uint32_t index = 0; index < 16; index += 2) {
        if (index) cmd_print(":", CMD_WHITE);
        cmd_print_hex_byte(address[index], CMD_YELLOW);
        cmd_print_hex_byte(address[index + 1], CMD_YELLOW);
    }
}

void userland_main(uint64_t argc, char **argv) {
    (void)argv;
    if (argc != 1) {
        cmd_print("usage: netinfo\n", CMD_YELLOW);
        return;
    }
    struct welovec_network_info info;
    const int64_t result = welovec_network_get_info(&info);
    if (result < 0) {
        cmd_print_error("netinfo", result);
        return;
    }
    cmd_print("Ethernet MAC: ", CMD_GREEN);
    for (uint32_t index = 0; index < sizeof(info.mac); ++index) {
        if (index) cmd_print(":", CMD_WHITE);
        cmd_print_hex_byte(info.mac[index], CMD_WHITE);
    }
    cmd_print("\nIPv4: ", CMD_GREEN);
    if (info.flags & WELOVEC_NET_INFO_IPV4_READY) print_ipv4(info.ipv4);
    else cmd_print("not configured", CMD_YELLOW);
    cmd_print("\nSubnet: ", CMD_GREEN);
    print_ipv4(info.subnet_mask);
    cmd_print("\nGateway: ", CMD_GREEN);
    print_ipv4(info.gateway);
    cmd_print("\nDNS: ", CMD_GREEN);
    print_ipv4(info.dns_server);
    cmd_print("\nDHCP: ", CMD_GREEN);
    const char *dhcp_status = "not configured";
    if (info.flags & WELOVEC_NET_INFO_DHCP) dhcp_status = "bound";
    else if (info.flags & WELOVEC_NET_INFO_IPV4_READY) dhcp_status = "static";
    else if (info.dhcp_phase != 0) dhcp_status = "unavailable";
    cmd_print(dhcp_status, CMD_WHITE);
    cmd_print("\nPackets sent/received: ", CMD_GREEN);
    cmd_print_number(info.ipv4_packets_sent, CMD_WHITE);
    cmd_print("/", CMD_WHITE);
    cmd_print_number(info.ipv4_packets_received, CMD_WHITE);
    cmd_print("\n", CMD_WHITE);

    struct welovec_network_ipv6_info ipv6;
    const int64_t ipv6_result = welovec_network_get_ipv6_info(&ipv6);
    if (ipv6_result < 0) {
        cmd_print_error("IPv6 status", ipv6_result);
        return;
    }
    cmd_print("IPv6 link-local: ", CMD_GREEN);
    print_ipv6(ipv6.link_local);
    cmd_print("\nIPv6 global: ", CMD_GREEN);
    if (ipv6.flags & WELOVEC_NET_IPV6_GLOBAL_READY)
        print_ipv6(ipv6.global_address);
    else cmd_print("waiting for router advertisement", CMD_YELLOW);
    cmd_print("\nIPv6 router: ", CMD_GREEN);
    if (ipv6.flags & WELOVEC_NET_IPV6_ROUTER_READY)
        print_ipv6(ipv6.default_router);
    else cmd_print("not available", CMD_YELLOW);
    cmd_print("\n", CMD_WHITE);
}
