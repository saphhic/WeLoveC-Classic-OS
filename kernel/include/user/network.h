// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares network operations available through system calls.

#ifndef WELOVEC_USER_NETWORK_H
#define WELOVEC_USER_NETWORK_H

#include <stdint.h>
#include "user/syscall.h"

#define WELOVEC_NET_INFO_INITIALIZED UINT32_C(1)
#define WELOVEC_NET_INFO_IPV4_READY UINT32_C(2)
#define WELOVEC_NET_INFO_DHCP UINT32_C(4)
#define WELOVEC_NET_IPV6_CONFIGURED UINT32_C(1)
#define WELOVEC_NET_IPV6_GLOBAL_READY UINT32_C(2)
#define WELOVEC_NET_IPV6_ROUTER_READY UINT32_C(4)
#define WELOVEC_TCP_MAX_CONNECTIONS 8u
#define WELOVEC_TCP_MAX_IO 1400u

struct welovec_network_info {
    uint32_t flags;
    uint32_t dhcp_phase;
    uint8_t mac[6];
    uint8_t ipv4[4];
    uint8_t subnet_mask[4];
    uint8_t gateway[4];
    uint8_t dns_server[4];
    uint32_t arp_packets_received;
    uint32_t ipv4_packets_received;
    uint32_t ipv4_packets_sent;
    uint32_t icmp_echo_requests_sent;
    uint32_t icmp_echo_replies_received;
};

struct welovec_network_ipv6_info {
    uint32_t flags;
    uint8_t link_local[16];
    uint8_t global_address[16];
    uint8_t prefix[16];
    uint8_t default_router[16];
};

static inline int64_t welovec_network_get_ipv6_info(
        struct welovec_network_ipv6_info *info) {
    return welovec_user_syscall1(WELOVEC_SYS_NET_IPV6_INFO,
                                 (uint64_t)(uintptr_t)info);
}

static inline int64_t welovec_network_get_info(struct welovec_network_info *info) {
    return welovec_user_syscall1(WELOVEC_SYS_NET_INFO,
                                 (uint64_t)(uintptr_t)info);
}

/* address is four bytes in network order; returns 0 on reply, -errno on failure. */
static inline int64_t welovec_ping_ipv4(const uint8_t address[4]) {
    return welovec_user_syscall1(WELOVEC_SYS_NET_PING,
                                 (uint64_t)(uintptr_t)address);
}

/* Resolves an ASCII hostname or copies an IPv4 literal into address_out. */
static inline int64_t welovec_dns_lookup_ipv4(const char *hostname,
                                              uint8_t address_out[4]) {
    return welovec_user_syscall2(WELOVEC_SYS_NET_DNS,
                                 (uint64_t)(uintptr_t)hostname,
                                 (uint64_t)(uintptr_t)address_out);
}

/* Resolves an ASCII hostname to its AAAA record. */
static inline int64_t welovec_dns_lookup_ipv6(const char *hostname,
                                              uint8_t address_out[16]) {
    return welovec_user_syscall2(WELOVEC_SYS_NET_DNS_IPV6,
                                 (uint64_t)(uintptr_t)hostname,
                                 (uint64_t)(uintptr_t)address_out);
}

/* Connects to IPv4:port. The returned socket handle belongs to this process;
   failures return negative errno values such as -EHOSTUNREACH, -EAGAIN,
   -ECONNREFUSED, -ETIMEDOUT, or -EIO. */
static inline int64_t welovec_tcp_connect_ipv4(const uint8_t address[4],
                                               uint16_t port) {
    return welovec_user_syscall2(WELOVEC_SYS_NET_TCP_CONNECT,
                                 (uint64_t)(uintptr_t)address, port);
}

/* Connects to IPv6:port with the same socket and error convention as IPv4. */
static inline int64_t welovec_tcp_connect_ipv6(const uint8_t address[16],
                                               uint16_t port) {
    return welovec_user_syscall2(WELOVEC_SYS_NET_TCP_CONNECT_IPV6,
                                 (uint64_t)(uintptr_t)address, port);
}

/* TCP writes are limited to WELOVEC_TCP_MAX_IO bytes per call. */
static inline int64_t welovec_tcp_send(int socket, const void *data,
                                       uint64_t size) {
    return welovec_user_syscall3(WELOVEC_SYS_NET_TCP_SEND, (uint64_t)socket,
                                 (uint64_t)(uintptr_t)data, size);
}

/* Nonblocking: returns 0 if no bytes are queued yet. */
static inline int64_t welovec_tcp_receive(int socket, void *data,
                                          uint64_t capacity) {
    return welovec_user_syscall3(WELOVEC_SYS_NET_TCP_RECV, (uint64_t)socket,
                                 (uint64_t)(uintptr_t)data, capacity);
}

static inline int64_t welovec_tcp_close(int socket) {
    return welovec_user_syscall1(WELOVEC_SYS_NET_TCP_CLOSE,
                                 (uint64_t)socket);
}

#endif
