// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Connects the Mbed TLS client to the system network operations.

#include "tls_client.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "user/filesystem.h"
#include "user/network.h"
#include "user/syscall.h"
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#define TLS_CA_BUNDLE_PATH "/userland/certs/ca-bundle.pem"
#define TLS_CA_BUNDLE_MAX (512u * 1024u)
#define TLS_OPERATION_TIMEOUT_TICKS 6000u

struct welovec_tls_session {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    int socket;
    int ssl_initialized;
    int config_initialized;
    int handshake_complete;
};

static mbedtls_x509_crt trusted_roots;
static int trusted_roots_state;
static int trusted_roots_error;

static void wipe_memory(void *memory, size_t size) {
    volatile uint8_t *bytes = (volatile uint8_t *)memory;
    while (size) {
        *bytes++ = 0;
        --size;
    }
}

static int load_trusted_roots(void) {
    if (trusted_roots_state) return trusted_roots_state > 0 ? 0 : trusted_roots_error;

    const psa_status_t psa_status = psa_crypto_init();
    if (psa_status != PSA_SUCCESS) {
        trusted_roots_state = -1;
        trusted_roots_error = (int)psa_status;
        return trusted_roots_error;
    }

    mbedtls_x509_crt_init(&trusted_roots);

    const int64_t file = welovec_open(TLS_CA_BUNDLE_PATH, WELOVEC_OPEN_READ);
    if (file < 0) {
        trusted_roots_state = -1;
        trusted_roots_error = MBEDTLS_ERR_X509_FILE_IO_ERROR;
        return trusted_roots_error;
    }

    unsigned char *bundle = (unsigned char *)malloc(TLS_CA_BUNDLE_MAX + 1u);
    if (!bundle) {
        (void)welovec_close((int)file);
        trusted_roots_state = -1;
        trusted_roots_error = MBEDTLS_ERR_SSL_ALLOC_FAILED;
        return trusted_roots_error;
    }

    size_t used = 0;
    int failed = 0;
    int failure_code = MBEDTLS_ERR_X509_INVALID_FORMAT;
    for (;;) {
        if (used == TLS_CA_BUNDLE_MAX) {
            failed = 1;
            break;
        }
        const int64_t count = welovec_read((int)file, bundle + used,
                                           TLS_CA_BUNDLE_MAX - used);
        if (count < 0) {
            failed = 1;
            failure_code = MBEDTLS_ERR_X509_FILE_IO_ERROR;
            break;
        }
        if (!count) break;
        used += (size_t)count;
    }
    (void)welovec_close((int)file);

    if (!failed && used) {
        bundle[used] = 0;
        const int parse_result =
            mbedtls_x509_crt_parse(&trusted_roots, bundle, used + 1u);
        failed = parse_result != 0;
        if (parse_result < 0) failure_code = parse_result;
    } else {
        failed = 1;
    }

    wipe_memory(bundle, TLS_CA_BUNDLE_MAX + 1u);
    free(bundle);
    if (failed || !trusted_roots.raw.p) {
        mbedtls_x509_crt_free(&trusted_roots);
        trusted_roots_state = -1;
        trusted_roots_error = failure_code;
        return trusted_roots_error;
    }

    trusted_roots_state = 1;
    return 0;
}

static int network_send(void *context, const unsigned char *buffer, size_t size) {
    struct welovec_tls_session *session =
        (struct welovec_tls_session *)context;
    if (!session || (!buffer && size)) return MBEDTLS_ERR_NET_BAD_INPUT_DATA;
    if (size > WELOVEC_TCP_MAX_IO) size = WELOVEC_TCP_MAX_IO;
    if (!size) return 0;

    const int64_t sent = welovec_tcp_send(session->socket, buffer, size);
    if (sent < 0) return MBEDTLS_ERR_NET_SEND_FAILED;
    if (!sent) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return (int)sent;
}

static int network_receive(void *context, unsigned char *buffer, size_t size) {
    struct welovec_tls_session *session =
        (struct welovec_tls_session *)context;
    if (!session || (!buffer && size)) return MBEDTLS_ERR_NET_BAD_INPUT_DATA;
    if (size > WELOVEC_TCP_MAX_IO) size = WELOVEC_TCP_MAX_IO;
    if (!size) return 0;

    const int64_t received = welovec_tcp_receive(session->socket, buffer, size);
    if (received < 0) return MBEDTLS_ERR_NET_RECV_FAILED;
    if (!received) return MBEDTLS_ERR_SSL_WANT_READ;
    return (int)received;
}

static int operation_timed_out(uint64_t start_tick) {
    return welovec_getticks() - start_tick >= TLS_OPERATION_TIMEOUT_TICKS;
}

struct welovec_tls_session *welovec_tls_connect(int socket,
                                                const char *hostname,
                                                int *error) {
    if (error) *error = MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    if (socket <= 0 || !hostname || !hostname[0])
        return 0;
    const int trust_error = load_trusted_roots();
    if (trust_error < 0) {
        if (error) *error = trust_error;
        return 0;
    }

    struct welovec_tls_session *session =
        (struct welovec_tls_session *)calloc(1, sizeof(*session));
    if (!session) {
        if (error) *error = MBEDTLS_ERR_SSL_ALLOC_FAILED;
        return 0;
    }
    session->socket = socket;
    mbedtls_ssl_config_init(&session->config);
    session->config_initialized = 1;
    mbedtls_ssl_init(&session->ssl);
    session->ssl_initialized = 1;

    int result = mbedtls_ssl_config_defaults(
        &session->config, MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (result == 0) {
        mbedtls_ssl_conf_min_tls_version(&session->config,
                                        MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_authmode(&session->config, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&session->config, &trusted_roots, 0);
        result = mbedtls_ssl_setup(&session->ssl, &session->config);
    }
    if (result == 0) result = mbedtls_ssl_set_hostname(&session->ssl, hostname);
    if (result == 0) {
        mbedtls_ssl_set_bio(&session->ssl, session, network_send,
                            network_receive, 0);
        const uint64_t start_tick = welovec_getticks();
        do {
            result = mbedtls_ssl_handshake(&session->ssl);
            if (result == 0) break;
            if (result != MBEDTLS_ERR_SSL_WANT_READ &&
                result != MBEDTLS_ERR_SSL_WANT_WRITE) break;
            if (operation_timed_out(start_tick)) {
                result = MBEDTLS_ERR_SSL_TIMEOUT;
                break;
            }
            (void)welovec_sleep(1);
        } while (1);
    }
    if (result == 0 && mbedtls_ssl_get_verify_result(&session->ssl) != 0)
        result = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
    if (result != 0) {
        if (error) *error = result;
        welovec_tls_close(session);
        return 0;
    }

    session->handshake_complete = 1;
    if (error) *error = 0;
    return session;
}

int welovec_tls_send(struct welovec_tls_session *session,
                     const void *data, size_t size) {
    if (!session || (!data && size)) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    if (!size) return 0;

    size_t sent = 0;
    uint64_t start_tick = welovec_getticks();
    while (sent < size) {
        const size_t remaining = size - sent;
        const int result = mbedtls_ssl_write(&session->ssl,
            (const unsigned char *)data + sent, remaining);
        if (result > 0) {
            sent += (size_t)result;
            start_tick = welovec_getticks();
            continue;
        }
        if (result != MBEDTLS_ERR_SSL_WANT_READ &&
            result != MBEDTLS_ERR_SSL_WANT_WRITE) return result;
        if (operation_timed_out(start_tick)) return MBEDTLS_ERR_SSL_TIMEOUT;
        (void)welovec_sleep(1);
    }
    return (int)sent;
}

int welovec_tls_receive(struct welovec_tls_session *session,
                        void *data, size_t capacity) {
    if (!session || (!data && capacity)) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    if (!capacity) return 0;

    const uint64_t start_tick = welovec_getticks();
    for (;;) {
        const int result = mbedtls_ssl_read(&session->ssl, (unsigned char *)data,
                                            capacity);
        if (result >= 0) return result;
        if (result == MBEDTLS_ERR_SSL_WANT_READ) return 0;
        if (result != MBEDTLS_ERR_SSL_WANT_WRITE) return result;
        if (operation_timed_out(start_tick)) return MBEDTLS_ERR_SSL_TIMEOUT;
        (void)welovec_sleep(1);
    }
}

void welovec_tls_close(struct welovec_tls_session *session) {
    if (!session) return;
    if (session->ssl_initialized) {
        if (session->handshake_complete)
            (void)mbedtls_ssl_close_notify(&session->ssl);
        mbedtls_ssl_free(&session->ssl);
    }
    if (session->config_initialized) mbedtls_ssl_config_free(&session->config);
    wipe_memory(session, sizeof(*session));
    free(session);
}

const char *welovec_tls_error_description(int error) {
    if (error == PSA_ERROR_INSUFFICIENT_ENTROPY || error == 148)
        return "no hay entropia criptografica; habilita EFI RNG, RDSEED/RDRAND o conecta un dispositivo virtio-rng a la VM";
    if (error == PSA_ERROR_INVALID_SIGNATURE || error == 149)
        return "la firma criptografica recibida no coincide; el certificado o la cadena enviados por el servidor pueden estar corruptos o ser incompatibles";
    if (error == MBEDTLS_ERR_X509_FILE_IO_ERROR || error == 10496)
        return "no se pudo leer /userland/certs/ca-bundle.pem";
    if (error == MBEDTLS_ERR_X509_INVALID_FORMAT)
        return "el archivo de certificados CA tiene un formato incorrecto o está incompleto";
    if (error == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED)
        return "el certificado no supera la validacion de confianza o nombre del servidor";
    if (error == MBEDTLS_ERR_SSL_TIMEOUT)
        return "se agotó el tiempo de espera durante la negociacion TLS";
    return 0;
}
