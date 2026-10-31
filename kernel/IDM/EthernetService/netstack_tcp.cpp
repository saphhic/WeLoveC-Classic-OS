// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements TCP for the network stack.

// netstack_tcp.cpp — TCP mínimo para KryptonOS
// Implementa 3-way handshake, envío de datos, recepción y cierre.
// Se apoya en ethernet_send / ethernet_receive del driver existente.
//
// Coloca este archivo en: KryptonKernel/drivers/ethernet/
// Compila con LAWAI_TARGET_BIOS o LAWAI_TARGET_UEFI.

#include "netstack_tcp.h"
#include "netstack.h"
#include "netstack_ipv6.h"
#include "ethernet.h"
#include "arch/x86_64/scheduler/scheduler.hpp"

extern "C" void* memcpy(void* dst, const void* src, unsigned long long n);
extern "C" void* memset(void* dst, int v, unsigned long long n);

// ── Constantes TCP/IP ──────────────────────────────────────

static constexpr uint8_t  kIpProtoTcp    = 6;
static constexpr uint16_t kTcpHdrLen     = 20;
static constexpr uint8_t  kTcpFlagFIN    = 0x01;
static constexpr uint8_t  kTcpFlagSYN    = 0x02;
static constexpr uint8_t  kTcpFlagRST    = 0x04;
static constexpr uint8_t  kTcpFlagPSH    = 0x08;
static constexpr uint8_t  kTcpFlagACK    = 0x10;
static constexpr uint64_t kConnectWaitTicks = 300;
static constexpr uint16_t kBaseEphemeral = 49152;
static constexpr uint16_t kMSS          = 1400;

// ── Utilidades byte-order ──────────────────────────────────

static inline uint16_t read_be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
static inline uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) <<  8) |
            static_cast<uint32_t>(p[3]);
}
static inline void write_be16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}
static inline void write_be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >>  8);
    p[3] = static_cast<uint8_t>(v);
}
static void cp4(uint8_t* d, const uint8_t* s)  { d[0]=s[0];d[1]=s[1];d[2]=s[2];d[3]=s[3]; }
static void cp6(uint8_t* d, const uint8_t* s)  { d[0]=s[0];d[1]=s[1];d[2]=s[2];d[3]=s[3];d[4]=s[4];d[5]=s[5]; }
static void cp16(uint8_t* d, const uint8_t* s) { memcpy(d, s, 16); }
static bool ip4eq(const uint8_t* a, const uint8_t* b) { return a[0]==b[0]&&a[1]==b[1]&&a[2]==b[2]&&a[3]==b[3]; }
static bool ip6eq(const uint8_t* a, const uint8_t* b) {
    for (int i = 0; i < 16; ++i) if (a[i] != b[i]) return false;
    return true;
}

// ── Checksum ───────────────────────────────────────────────

static uint16_t ip_ones_complement(const uint8_t* data, uint16_t len) {
    uint32_t acc = 0;
    for (uint16_t i = 0; i+1 < len; i += 2)
        acc += static_cast<uint32_t>(read_be16(&data[i]));
    if (len & 1) acc += static_cast<uint32_t>(data[len-1]) << 8;
    while (acc >> 16) acc = (acc & 0xFFFFu) + (acc >> 16);
    return static_cast<uint16_t>(~acc);
}

static uint16_t tcp_checksum(const uint8_t* src, const uint8_t* dst,
                             const uint8_t* seg, uint16_t seg_len,
                             bool ipv6) {
    uint32_t acc = 0;
    if (ipv6) {
        for (int i = 0; i < 16; i += 2) {
            acc += read_be16(&src[i]);
            acc += read_be16(&dst[i]);
        }
        acc += seg_len;
        acc += kIpProtoTcp;
    } else {
        uint8_t ph[12];
        cp4(&ph[0], src); cp4(&ph[4], dst);
        ph[8]=0; ph[9]=kIpProtoTcp; write_be16(&ph[10], seg_len);
        for (int i = 0; i < 12; i+=2) acc += read_be16(&ph[i]);
    }
    for (uint16_t i = 0; i+1 < seg_len; i+=2) acc += read_be16(&seg[i]);
    if (seg_len & 1) acc += static_cast<uint32_t>(seg[seg_len-1]) << 8;
    while (acc >> 16) acc = (acc & 0xFFFFu) + (acc >> 16);
    return static_cast<uint16_t>(~acc);
}

// ── Estado de conexión ─────────────────────────────────────

enum TcpState : uint8_t {
    TCP_CLOSED=0, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_FIN_WAIT, TCP_TIME_WAIT
};

struct TcpConn {
    TcpState state;
    bool ipv6;
    uint8_t  local_ip[16], remote_ip[16], remote_mac[6];
    uint16_t local_port, remote_port;
    uint32_t seq, ack;
    uint8_t  recv_buf[TCP_RECV_BUF_SIZE];
    int      recv_head, recv_tail;
};

static int tcp_recv_buf_free_space(const TcpConn* c) {
    if (c->recv_tail >= c->recv_head) {
        return TCP_RECV_BUF_SIZE - 1 - (c->recv_tail - c->recv_head);
    }
    return c->recv_head - c->recv_tail - 1;
}

static TcpConn   s_conns[TCP_MAX_CONNS];
static bool      s_init      = false;
static uint16_t  s_next_port = kBaseEphemeral;
static uint16_t  s_ip_id     = 1;

static void tcp_init_once() {
    if (s_init) return;
    s_init = true;
    memset(s_conns, 0, sizeof(s_conns));
}
static int tcp_alloc() {
    for (int i = 0; i < TCP_MAX_CONNS; ++i)
        if (s_conns[i].state == TCP_CLOSED) {
            memset(&s_conns[i], 0, sizeof(TcpConn));
            return i;
        }
    return -1;
}

// ── Envío de segmento TCP+IP ───────────────────────────────

static bool tcp_send_seg(TcpConn* c, uint8_t flags,
                         const uint8_t* payload, uint16_t plen) {
    // Calcular el espacio libre en el buffer de recepción para anunciarlo (control de flujo)
    int free_space = tcp_recv_buf_free_space(c);
    uint16_t window_size = (free_space > 0) ? static_cast<uint16_t>(free_space) : 0;

    // Construir segmento TCP (sin opciones)
    const uint16_t seg_len = static_cast<uint16_t>(kTcpHdrLen + plen);
    uint8_t seg[kTcpHdrLen + kMSS];
    if (plen > kMSS) return false;
    memset(seg, 0, kTcpHdrLen);
    write_be16(&seg[0],  c->local_port);
    write_be16(&seg[2],  c->remote_port);
    write_be32(&seg[4],  c->seq);
    write_be32(&seg[8],  c->ack);
    seg[12] = static_cast<uint8_t>((kTcpHdrLen/4) << 4);
    seg[13] = flags;
    write_be16(&seg[14], window_size); // Anunciar nuestra ventana de recepción
    if (payload && plen) memcpy(&seg[kTcpHdrLen], payload, plen);
    uint16_t checksum = tcp_checksum(c->local_ip, c->remote_ip, seg,
                                     seg_len, c->ipv6);
    if (!checksum) checksum = 0xFFFFu;
    write_be16(&seg[16], checksum);

    if (c->ipv6) {
        return netstack_ipv6_send_packet(c->remote_mac, c->local_ip,
                                         c->remote_ip, kIpProtoTcp, 64,
                                         seg, seg_len);
    }

    const uint16_t ip_total = static_cast<uint16_t>(20 + seg_len);
    uint8_t frame[20 + kTcpHdrLen + kMSS];
    memset(frame, 0, 20);
    frame[0] = 0x45;
    write_be16(&frame[2],  ip_total);
    write_be16(&frame[4],  s_ip_id++);
    frame[6]  = 0x40;
    frame[8]  = 64;
    frame[9]  = kIpProtoTcp;
    cp4(&frame[12], c->local_ip);
    cp4(&frame[16], c->remote_ip);
    write_be16(&frame[10], ip_ones_complement(frame, 20));
    memcpy(&frame[20], seg, seg_len);
    return ethernet_send(c->remote_mac, 0x0800, frame, ip_total);
}

// ── Procesamiento de frame TCP entrante ───────────────────

void tcp_handle_packet(const ethernet_packet* pkt) {
    if (pkt == nullptr) return;
    const uint8_t* ip = pkt->payload;
    bool ipv6 = false;
    const uint8_t* src_ip = nullptr;
    const uint8_t* dst_ip = nullptr;
    const uint8_t* tcp = nullptr;
    uint16_t tcp_len = 0;
    if (pkt->ether_type == 0x0800) {
        const uint16_t ip_len = pkt->payload_length;
        if (ip_len < 20 || ip[9] != kIpProtoTcp) return;
        const uint8_t ihl = static_cast<uint8_t>((ip[0] & 0x0F) * 4);
        if (ihl < 20 || ihl > ip_len) return;
        const uint16_t total_length = read_be16(&ip[2]);
        if (total_length < ihl || total_length > ip_len) return;
        src_ip = &ip[12];
        dst_ip = &ip[16];
        tcp = ip + ihl;
        tcp_len = static_cast<uint16_t>(total_length - ihl);
    } else if (pkt->ether_type == 0x86DD) {
        ipv6 = true;
        if (pkt->payload_length < 40 || (ip[0] >> 4) != 6 || ip[6] != kIpProtoTcp)
            return;
        const uint16_t payload_length = read_be16(&ip[4]);
        if (static_cast<uint32_t>(payload_length) + 40 > pkt->payload_length)
            return;
        src_ip = &ip[8];
        dst_ip = &ip[24];
        tcp = ip + 40;
        tcp_len = payload_length;
    } else {
        return;
    }
    if (tcp_len < kTcpHdrLen) return;
    if (tcp_checksum(src_ip, dst_ip, tcp, tcp_len, ipv6) != 0) return;

    const uint16_t sp  = read_be16(&tcp[0]);
    const uint16_t dp  = read_be16(&tcp[2]);
    const uint32_t seq = read_be32(&tcp[4]);
    const uint32_t ack = read_be32(&tcp[8]);
    const uint8_t  doff= static_cast<uint8_t>((tcp[12]>>4)*4);
    const uint8_t  fl  = tcp[13];
    if (doff < kTcpHdrLen || doff > tcp_len) return;
    const uint8_t* data    = tcp + doff;
    const uint16_t data_len= static_cast<uint16_t>(tcp_len - doff);

    for (int i = 0; i < TCP_MAX_CONNS; ++i) {
        TcpConn* c = &s_conns[i];
        if (c->state == TCP_CLOSED) continue;
        if (c->ipv6 != ipv6) continue;
        if (c->remote_port != sp) continue;
        if (c->local_port  != dp) continue;
        if (ipv6 ? !ip6eq(c->remote_ip, src_ip) : !ip4eq(c->remote_ip, src_ip)) continue;

        if (c->state == TCP_SYN_SENT) {
            if ((fl & (kTcpFlagSYN|kTcpFlagACK)) == (kTcpFlagSYN|kTcpFlagACK)) {
                c->ack = seq + 1;
                c->seq = ack;
                c->state = TCP_ESTABLISHED;
                tcp_send_seg(c, kTcpFlagACK, nullptr, 0);
            }
            return;
        }

        if (c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT) {
            if (fl & kTcpFlagRST) { c->state = TCP_CLOSED; return; }

            // Solo aceptar el segmento esperado para evitar reordenamiento simple
            if (seq != c->ack) {
                // Si no es el esperado, al menos enviamos un ACK para que el otro lado sepa qué esperamos
                tcp_send_seg(c, kTcpFlagACK, nullptr, 0);
                return;
            }

            if (data_len > 0) {
                int free_space = tcp_recv_buf_free_space(c);
                for (uint16_t b = 0; b < data_len; ++b) {
                    if (free_space > 0) {
                        c->recv_buf[c->recv_tail] = data[b];
                        c->recv_tail = (c->recv_tail + 1) % TCP_RECV_BUF_SIZE;
                        free_space--;
                    }
                }
                c->ack += data_len;
            }

            tcp_send_seg(c, kTcpFlagACK, nullptr, 0);
            if (fl & kTcpFlagFIN) {
                c->ack++;
                tcp_send_seg(c, kTcpFlagACK, nullptr, 0);
                c->state = TCP_TIME_WAIT;
            }
        }
        return;
    }
}

// ── Polling ────────────────────────────────────────────────

void tcp_poll() {
    // La netstack posee la cola RX y distribuye los paquetes TCP.
    netstack_poll();
}

// ── API pública ────────────────────────────────────────────

static int tcp_connect_resolved(const uint8_t source_ip[16],
                                const uint8_t dest_ip[16],
                                const uint8_t dest_mac[6],
                                uint16_t dest_port, bool ipv6) {
    tcp_init_once();

    int cid = tcp_alloc();
    if (cid < 0) return -1;

    TcpConn* c = &s_conns[cid];
    c->ipv6 = ipv6;
    if (ipv6) {
        cp16(c->local_ip, source_ip);
        cp16(c->remote_ip, dest_ip);
    } else {
        cp4(c->local_ip, source_ip);
        cp4(c->remote_ip, dest_ip);
    }
    cp6(c->remote_mac, dest_mac);
    c->local_port  = s_next_port++;
    if (s_next_port == 0) s_next_port = kBaseEphemeral;
    c->remote_port = dest_port;
    c->seq         = 0x4C410001u;
    c->ack         = 0;
    c->recv_head   = c->recv_tail = 0;
    c->state       = TCP_SYN_SENT;

    // SYN
    if (!tcp_send_seg(c, kTcpFlagSYN, nullptr, 0)) { c->state=TCP_CLOSED; return -1; }
    c->seq++;

    // Esperar SYN-ACK
    const uint64_t deadline =
        welovec::x86_64::scheduler::tick_count() + kConnectWaitTicks;
    while (welovec::x86_64::scheduler::tick_count() < deadline) {
        tcp_poll();
        if (c->state == TCP_ESTABLISHED) return cid;
        if (c->state == TCP_CLOSED)      return -1;
        (void)welovec::x86_64::scheduler::sleep(1);
    }
    c->state = TCP_CLOSED;
    return -1;
}

int tcp_connect(const uint8_t dest_ip[4], uint16_t dest_port) {
    if (!dest_ip) return -1;
    netstack_info info = {};
    netstack_get_info(&info);
    if (!info.initialized || !info.ipv4_configured) return -1;
    uint8_t source[16] = {};
    uint8_t destination[16] = {};
    uint8_t mac[6] = {};
    cp4(source, info.ip);
    cp4(destination, dest_ip);
    if (!netstack_resolve_ipv4_mac(dest_ip, mac)) return -1;
    return tcp_connect_resolved(source, destination, mac, dest_port, false);
}

int tcp_connect_ipv6(const uint8_t dest_ip[16], uint16_t dest_port) {
    if (!dest_ip) return -1;
    netstack_ipv6_info info{};
    netstack_ipv6_get_info(&info);
    if (!info.configured) return -1;
    uint8_t source[16] = {};
    uint8_t mac[6] = {};
    if (!netstack_ipv6_resolve(dest_ip, source, mac)) return -1;
    return tcp_connect_resolved(source, dest_ip, mac, dest_port, true);
}

bool tcp_send(int id, const uint8_t* data, uint16_t len) {
    if (id < 0 || id >= TCP_MAX_CONNS) return false;
    TcpConn* c = &s_conns[id];
    if (c->state != TCP_ESTABLISHED) return false;

    uint16_t sent = 0;
    while (sent < len) {
        uint16_t chunk = static_cast<uint16_t>(len - sent);
        if (chunk > kMSS) chunk = kMSS;
        if (!tcp_send_seg(c, kTcpFlagACK|kTcpFlagPSH, data+sent, chunk)) return false;
        c->seq = static_cast<uint32_t>(c->seq + chunk);
        sent   = static_cast<uint16_t>(sent + chunk);
    }
    return true;
}

int tcp_recv(int id,
             uint8_t* buf,
             int buf_size,
             uint32_t max_loops)
{
    if (id < 0 || id >= TCP_MAX_CONNS)
        return 0;

    TcpConn* c = &s_conns[id];

    int got = 0;

    for (uint32_t spin = 0;
         spin < max_loops;
         ++spin)
    {
        tcp_poll();

        while (got < buf_size &&
               c->recv_head != c->recv_tail)
        {
            buf[got++] =
                c->recv_buf[c->recv_head];

            c->recv_head =
                (c->recv_head + 1)
                % TCP_RECV_BUF_SIZE;
        }

        // Si ya hemos copiado datos al buffer del usuario, retornamos inmediatamente.
        // La aplicación puede volver a llamar a tcp_recv si necesita más datos.
        if (got > 0) {
            break;
        }

        if (c->state == TCP_TIME_WAIT ||
            c->state == TCP_CLOSED)
        {
            break;
        }
    }

    return got;
}

void tcp_close(int id) {
    if (id < 0 || id >= TCP_MAX_CONNS) return;
    TcpConn* c = &s_conns[id];
    if (c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT) {
        c->state = TCP_FIN_WAIT;
        tcp_send_seg(c, kTcpFlagFIN|kTcpFlagACK, nullptr, 0);
        c->seq++;
    }
    c->state = TCP_CLOSED;
}
