// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for netstack tcp in the ethernet service module.

// netstack_tcp.h — TCP mínimo para KryptonOS
// Implementa conexión TCP básica (connect / send / recv / close)
// suficiente para hacer peticiones HTTP/1.0 sin Keep-Alive.
//
// Uso típico:
//   int conn = tcp_connect(server_ip, 80);
//   if (conn < 0) { /* error */ }
//   tcp_send(conn, request, request_len);
//   int n = tcp_recv(conn, buffer, sizeof(buffer));
//   tcp_close(conn);

#ifndef KRYPTON_NETSTACK_TCP_H
#define KRYPTON_NETSTACK_TCP_H

#include <stdint.h>
#include <stdbool.h>

struct ethernet_packet;

// Tamaño máximo del buffer de recepción por conexión
#define TCP_RECV_BUF_SIZE  65536  // 64 KB es más que suficiente y ahorra memoria
// Número máximo de conexiones simultáneas
#define TCP_MAX_CONNS      8

// ── API pública ────────────────────────────────────────────

// Abre una conexión TCP a dest_ip:dest_port.
// Devuelve un conn_id >= 0, o -1 si falla.
int  tcp_connect(const uint8_t dest_ip[4], uint16_t dest_port);
int  tcp_connect_ipv6(const uint8_t dest_ip[16], uint16_t dest_port);

// Envía datos por la conexión. Devuelve true si OK.
bool tcp_send(int conn_id, const uint8_t* data, uint16_t length);

// Recibe datos disponibles. Devuelve número de bytes copiados en buf.
// Llama netstack_poll() internamente para procesar frames entrantes.
// max_wait_loops: iteraciones de polling antes de dar timeout.
int  tcp_recv(int conn_id, uint8_t* buf, int buf_size, uint32_t max_wait_loops);

// Cierra la conexión (envía FIN).
void tcp_close(int conn_id);

// Procesa frames TCP entrantes (llamar periódicamente desde el bucle principal).
void tcp_poll();
void tcp_handle_packet(const ethernet_packet* packet);

#endif // KRYPTON_NETSTACK_TCP_H
