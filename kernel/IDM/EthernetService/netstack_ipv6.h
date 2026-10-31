// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for netstack ipv6 in the ethernet service module.

#ifndef WELOVEC_NETSTACK_IPV6_H
#define WELOVEC_NETSTACK_IPV6_H

#include <stdint.h>

struct ethernet_packet;

struct netstack_ipv6_info {
    bool configured;
    bool has_global_address;
    bool has_default_router;
    uint8_t link_local[16];
    uint8_t global_address[16];
    uint8_t prefix[16];
    uint8_t default_router[16];
};

void netstack_ipv6_init(const uint8_t mac[6]);
void netstack_ipv6_poll();
void netstack_ipv6_handle_packet(const ethernet_packet* packet);
void netstack_ipv6_get_info(netstack_ipv6_info* out_info);
bool netstack_ipv6_parse(const char* text, uint8_t out_address[16]);
bool netstack_ipv6_resolve(const uint8_t destination[16],
                           uint8_t source[16], uint8_t out_mac[6]);
bool netstack_ipv6_send_packet(const uint8_t destination_mac[6],
                               const uint8_t source[16],
                               const uint8_t destination[16],
                               uint8_t next_header, uint8_t hop_limit,
                               const void* payload, uint16_t payload_length);

#endif
