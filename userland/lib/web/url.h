/* Written by [@saphhic](https://github.com/saphhic)
 * Date: October 31st 2026
 * Description: Declares bounded HTTP and HTTPS URL handling for userland.
 */

#ifndef WELOVEC_USERLAND_WEB_URL_H
#define WELOVEC_USERLAND_WEB_URL_H

#include <stdint.h>

#define WELOVEC_URL_HOST_CAPACITY 128u
#define WELOVEC_URL_PATH_CAPACITY 256u
#define WELOVEC_URL_MAX_LENGTH 1024u

struct welovec_url {
    char host[WELOVEC_URL_HOST_CAPACITY];
    char path[WELOVEC_URL_PATH_CAPACITY];
    uint16_t port;
    uint8_t secure;
    uint8_t ipv6_literal;
};

/* Parses absolute http:// and https:// URLs into a host, port, and request
   target. Hostnames are ASCII DNS labels; IDNA and percent canonicalization
   are not implemented. Returns 1 on success and 0 for malformed input. */
int welovec_url_parse(const char *text, struct welovec_url *out);

/* Writes an absolute URL, including brackets around IPv6 literals. */
int welovec_url_format(const struct welovec_url *url,
                       char output[WELOVEC_URL_MAX_LENGTH]);

/* Resolves an absolute, network-path, or relative HTTP(S) reference against
   base. Fragments are discarded because they are not sent in HTTP requests.
   Paths are bounded by WELOVEC_URL_PATH_CAPACITY. */
int welovec_url_resolve(const struct welovec_url *base, const char *reference,
                        struct welovec_url *out);

/* Compares scheme, effective port, and host using case-insensitive DNS
   comparison or binary IPv6 comparison. */
int welovec_url_same_origin(const struct welovec_url *left,
                            const struct welovec_url *right);

/* Parses an IPv6 literal without brackets into its 16-byte network form. */
int welovec_url_parse_ipv6(const char *text, uint8_t output[16]);

#endif
