// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for tls client in the tls module.

#ifndef WELOVEC_TLS_CLIENT_H
#define WELOVEC_TLS_CLIENT_H

#include <stddef.h>

struct welovec_tls_session;

/*
 * Create a TLS 1.2+ client session on an already connected WeLoveC TCP socket.
 * The server certificate and hostname are checked against the installed CA
 * bundle. On failure, *error receives a negative Mbed TLS or PSA error code.
 */
struct welovec_tls_session *welovec_tls_connect(int socket,
                                                const char *hostname,
                                                int *error);

/* Return transferred bytes, 0 when a read needs more network data, or a
 * negative Mbed TLS error. Both calls are safe for the 1400-byte TCP syscall
 * limit and can be retried after a WANT_READ/WANT_WRITE condition. */
int welovec_tls_send(struct welovec_tls_session *session,
                     const void *data, size_t size);
int welovec_tls_receive(struct welovec_tls_session *session,
                        void *data, size_t capacity);

/* Free the TLS state. The caller owns and closes the underlying TCP socket. */
void welovec_tls_close(struct welovec_tls_session *session);

/* Human-readable reason for a TLS setup or certificate validation error. */
const char *welovec_tls_error_description(int error);

#endif
