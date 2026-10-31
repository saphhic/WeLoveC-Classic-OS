/* Written by [@saphhic](https://github.com/saphhic)
 * Date: October 31st 2026
 * Description: Declares the bounded HTTP GET client for userland.
 */

#ifndef WELOVEC_USERLAND_WEB_HTTP_H
#define WELOVEC_USERLAND_WEB_HTTP_H

#include <stddef.h>
#include <stdint.h>

#include "url.h"

#define WELOVEC_HTTP_HEADER_MAX 16384u
#define WELOVEC_HTTP_REDIRECT_MAX 5u

enum welovec_http_error {
    WELOVEC_HTTP_OK = 0,
    WELOVEC_HTTP_INVALID_URL = -1,
    WELOVEC_HTTP_NETWORK_ERROR = -2,
    WELOVEC_HTTP_TLS_ERROR = -3,
    WELOVEC_HTTP_BAD_RESPONSE = -4,
    WELOVEC_HTTP_BODY_TOO_LARGE = -5,
    WELOVEC_HTTP_UNSUPPORTED_RESPONSE = -6,
    WELOVEC_HTTP_REDIRECT_ERROR = -7,
    WELOVEC_HTTP_TIMEOUT = -8,
    WELOVEC_HTTP_DNS_ERROR = -9,
    WELOVEC_HTTP_CONNECT_ERROR = -10,
    WELOVEC_HTTP_SEND_ERROR = -11,
    WELOVEC_HTTP_RECEIVE_ERROR = -12,
    WELOVEC_HTTP_CONNECT_TIMEOUT = -13,
    WELOVEC_HTTP_CONNECT_REFUSED = -14,
    WELOVEC_HTTP_CONNECT_NETWORK_ERROR = -15,
    WELOVEC_HTTP_CONNECT_ENTROPY_ERROR = -16,
    WELOVEC_HTTP_CONNECT_RESOURCE_ERROR = -17,
    WELOVEC_HTTP_CONNECT_SEND_ERROR = -18
};

struct welovec_http_response {
    uint16_t status;
    size_t body_size;
    char final_url[WELOVEC_URL_MAX_LENGTH];
    char content_type[128];
};

/* Performs a bounded GET. The caller supplies the body buffer and capacity.
   Supports Content-Length and chunked responses, and follows at most five
   redirects. An HTTPS redirect to HTTP is rejected. Credentials and cookies
   are never sent. `tls_error` receives the underlying TLS error when present. */
int welovec_http_get(const char *url, uint8_t *body, size_t body_capacity,
                     struct welovec_http_response *response, int *tls_error);

const char *welovec_http_error_string(int error);

#endif
