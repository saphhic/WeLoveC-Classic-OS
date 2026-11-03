// Written by [@saphhic](https://github.com/saphhic)
// Date: October 31st 2026
// Description: Declares the types, constants, and functions for netstack tcp in the ethernet service module.

/* Small stop-and-wait TCP API for the current network stack.
   It handles ordered data and bounded SYN/data retransmission, but does not
   implement congestion control, window scaling, or out-of-order buffering. */

#ifndef WELOVEC_IDM_ETHERNET_NETSTACK_TCP_H
#define WELOVEC_IDM_ETHERNET_NETSTACK_TCP_H

#include <stdint.h>
#include <stdbool.h>

struct ethernet_packet;

// Maximum receive-buffer size per connection.
#define TCP_RECV_BUF_SIZE  65536  // 64 KB is sufficient and keeps memory use low.
// Maximum number of simultaneous connections.
#define TCP_MAX_CONNS      8

// ── Public API ──────────────────────────────────────────────

/* Open a TCP connection to dest_ip:dest_port. Returns a connection ID or a
   negative errno (for example -EHOSTUNREACH, -EAGAIN, -ECONNREFUSED,
   -ETIMEDOUT, or -EIO). */
int  tcp_connect(const uint8_t dest_ip[4], uint16_t dest_port);
int  tcp_connect_ipv6(const uint8_t dest_ip[16], uint16_t dest_port);

/* Send all bytes and wait for their cumulative ACK; true means acknowledged. */
bool tcp_send(int conn_id, const uint8_t* data, uint16_t length);

/* Drain queued bytes into buf. max_loops controls optional stack polling. */
int  tcp_recv(int conn_id, uint8_t* buf, int buf_size, uint32_t max_wait_loops);

/* Send FIN when possible and release the transport slot. */
void tcp_close(int conn_id);

// Process incoming TCP frames; call periodically from the main loop.
void tcp_poll();
void tcp_handle_packet(const ethernet_packet* packet);

#endif // WELOVEC_IDM_ETHERNET_NETSTACK_TCP_H
