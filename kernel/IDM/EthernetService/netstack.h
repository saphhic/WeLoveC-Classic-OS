// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for netstack in the ethernet service module.

//written by saphhic.

#ifndef LAWAI_NETSTACK_H
#define LAWAI_NETSTACK_H

#include <stdint.h>

// Snapshot de la configuracion/estadisticas IPv4 actuales, pensado para que
// comandos de shell (o cualquier otro consumidor) lean el estado sin tener
// que parsear la salida de las funciones netstack_debug_*.
struct netstack_info {
    bool initialized;
    bool ipv4_configured;
    bool using_dhcp;
    uint8_t dhcp_phase;
    uint8_t mac[6];
    uint8_t ip[4];
    uint8_t subnet_mask[4];
    uint8_t gateway[4];
    uint8_t dns_server[4];
    uint32_t arp_packets_received;
    uint32_t ipv4_packets_received;
    uint32_t ipv4_packets_sent;
    uint32_t icmp_echo_requests_sent;
    uint32_t icmp_echo_replies_received;
};

bool netstack_init();
void netstack_poll();
void netstack_get_info(netstack_info* out_info);
bool netstack_ping(const uint8_t target_ip[4]);
bool netstack_resolve_ipv4_mac(const uint8_t target_ip[4], uint8_t out_mac[6]);
bool netstack_parse_ipv4(const char* text, uint8_t out_ip[4]);
bool netstack_dns_resolve(const char* hostname, uint8_t out_ip[4]);
bool netstack_dns_resolve_ipv6(const char* hostname, uint8_t out_ip[16]);
void netstack_debug_print_info();
void netstack_debug_use_qemu_user_defaults();
void netstack_debug_resolve_gateway();
void netstack_debug_ping_gateway();

#endif // LAWAI_NETSTACK_H
