/* Written by [@saphhic](https://github.com/saphhic)
 * Date: October 31st 2026
 * Description: Implements bounded HTTP and HTTPS URL parsing for userland.
 */

#include "url.h"

static char ascii_lower(char value) {
    return value >= 'A' && value <= 'Z' ? (char)(value - 'A' + 'a') : value;
}

static int equal_prefix(const char *text, const char *prefix) {
    uint32_t index = 0;
    while (prefix[index]) {
        if (!text[index] || ascii_lower(text[index]) != prefix[index]) return 0;
        ++index;
    }
    return 1;
}

static int copy_part(char *destination, uint32_t capacity,
                     const char *source, uint32_t length) {
    if (!capacity || length >= capacity) return 0;
    for (uint32_t index = 0; index < length; ++index)
        destination[index] = source[index];
    destination[length] = '\0';
    return 1;
}

static int append_char(char *destination, uint32_t capacity, uint32_t *length,
                       char value) {
    if (!destination || !length || *length + 1 >= capacity) return 0;
    destination[(*length)++] = value;
    destination[*length] = '\0';
    return 1;
}

static int append_part(char *destination, uint32_t capacity, uint32_t *length,
                       const char *source, uint32_t source_length) {
    if (!destination || !length || !source || *length >= capacity ||
        source_length >= capacity - *length) return 0;
    for (uint32_t index = 0; index < source_length; ++index)
        destination[(*length)++] = source[index];
    destination[*length] = '\0';
    return 1;
}

static uint32_t bounded_length(const char *text, uint32_t limit) {
    uint32_t length = 0;
    if (!text) return limit;
    while (length < limit && text[length]) ++length;
    return length;
}

static int has_prefix(const char *text, uint32_t length,
                      const char *prefix, uint32_t prefix_length) {
    if (length < prefix_length) return 0;
    for (uint32_t index = 0; index < prefix_length; ++index)
        if (text[index] != prefix[index]) return 0;
    return 1;
}

static int valid_request_target(const char *path) {
    if (!path || path[0] != '/') return 0;
    for (uint32_t index = 0; path[index]; ++index) {
        const uint8_t character = (uint8_t)path[index];
        if (character <= 0x20 || character == 0x7f || character == '\\') return 0;
    }
    return 1;
}

static int valid_hostname(const char *host, uint32_t length) {
    uint32_t label_length = 0;
    if (!length || length > WELOVEC_URL_HOST_CAPACITY - 1 ||
        host[0] == '.' || host[0] == '-') return 0;
    for (uint32_t index = 0; index < length; ++index) {
        const char character = host[index];
        if (character == '.') {
            if (!label_length || host[index - 1] == '-') return 0;
            label_length = 0;
            continue;
        }
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '-'))
            return 0;
        if (!label_length && character == '-') return 0;
        if (++label_length > 63) return 0;
    }
    return label_length && host[length - 1] != '-';
}

int welovec_url_parse_ipv6(const char *text, uint8_t output[16]) {
    uint8_t parsed[16] = {0};
    int compression = -1;
    uint8_t groups = 0;
    const char *cursor = text;
    if (!cursor || !*cursor || !output) return 0;
    if (*cursor == ':') {
        if (cursor[1] != ':') return 0;
        compression = 0;
        cursor += 2;
        if (!*cursor) {
            for (uint32_t index = 0; index < 16; ++index) output[index] = 0;
            return 1;
        }
    }
    while (*cursor) {
        if (groups >= 8) return 0;
        uint32_t value = 0;
        uint32_t digits = 0;
        while (*cursor && *cursor != ':') {
            const char character = *cursor++;
            uint32_t nibble;
            if (character >= '0' && character <= '9')
                nibble = (uint32_t)(character - '0');
            else if (character >= 'a' && character <= 'f')
                nibble = (uint32_t)(character - 'a' + 10);
            else if (character >= 'A' && character <= 'F')
                nibble = (uint32_t)(character - 'A' + 10);
            else
                return 0;
            if (++digits > 4) return 0;
            value = (value << 4) | nibble;
        }
        if (!digits) return 0;
        parsed[groups * 2] = (uint8_t)(value >> 8);
        parsed[groups * 2 + 1] = (uint8_t)value;
        ++groups;
        if (!*cursor) break;
        ++cursor;
        if (*cursor == ':') {
            if (compression >= 0) return 0;
            compression = groups;
            ++cursor;
            if (!*cursor) break;
        } else if (!*cursor) {
            return 0;
        }
    }
    if (compression >= 0) {
        if (groups >= 8) return 0;
        const uint8_t zeros = (uint8_t)(8 - groups);
        for (int index = (int)groups - 1; index >= compression; --index) {
            parsed[(index + zeros) * 2] = parsed[index * 2];
            parsed[(index + zeros) * 2 + 1] = parsed[index * 2 + 1];
        }
        for (uint8_t index = 0; index < zeros; ++index) {
            parsed[(compression + index) * 2] = 0;
            parsed[(compression + index) * 2 + 1] = 0;
        }
    } else if (groups != 8) {
        return 0;
    }
    for (uint32_t index = 0; index < 16; ++index) output[index] = parsed[index];
    return 1;
}

static int parse_port(const char *start, uint32_t length, uint16_t *port) {
    uint32_t parsed = 0;
    if (!length || !port) return 0;
    for (uint32_t index = 0; index < length; ++index) {
        if (start[index] < '0' || start[index] > '9') return 0;
        const uint32_t digit = (uint32_t)(start[index] - '0');
        if (parsed > (65535u - digit) / 10u) return 0;
        parsed = parsed * 10u + digit;
    }
    if (!parsed) return 0;
    *port = (uint16_t)parsed;
    return 1;
}

int welovec_url_parse(const char *text, struct welovec_url *out) {
    if (!text || !out) return 0;
    out->host[0] = '\0';
    out->path[0] = '\0';
    out->port = 0;
    out->secure = 0;
    out->ipv6_literal = 0;

    uint32_t text_length = 0;
    while (text_length < WELOVEC_URL_MAX_LENGTH && text[text_length]) ++text_length;
    if (!text_length || text_length == WELOVEC_URL_MAX_LENGTH) return 0;

    const char *authority = 0;
    if (equal_prefix(text, "http://")) {
        authority = text + 7;
        out->port = 80;
    } else if (equal_prefix(text, "https://")) {
        authority = text + 8;
        out->port = 443;
        out->secure = 1;
    } else {
        return 0;
    }

    uint32_t authority_length = 0;
    while (authority[authority_length] && authority[authority_length] != '/' &&
           authority[authority_length] != '?' && authority[authority_length] != '#')
        ++authority_length;
    if (!authority_length) return 0;

    uint32_t authority_cursor = 0;
    if (authority[0] == '[') {
        uint32_t close = 1;
        while (close < authority_length && authority[close] != ']') ++close;
        if (close == authority_length || close == 1 ||
            close - 1 >= WELOVEC_URL_HOST_CAPACITY ||
            !copy_part(out->host, sizeof(out->host), authority + 1, close - 1))
            return 0;
        uint8_t parsed_address[16];
        if (!welovec_url_parse_ipv6(out->host, parsed_address)) return 0;
        out->ipv6_literal = 1;
        authority_cursor = close + 1;
        if (authority_cursor < authority_length &&
            (authority[authority_cursor] != ':' ||
             !parse_port(authority + authority_cursor + 1,
                         authority_length - authority_cursor - 1, &out->port)))
            return 0;
    } else {
        uint32_t colon = authority_length;
        for (uint32_t index = 0; index < authority_length; ++index) {
            if (authority[index] == ':') {
                if (colon != authority_length) return 0;
                colon = index;
            }
        }
        if (!colon || colon >= WELOVEC_URL_HOST_CAPACITY ||
            !valid_hostname(authority, colon) ||
            !copy_part(out->host, sizeof(out->host), authority, colon))
            return 0;
        if (colon < authority_length &&
            !parse_port(authority + colon + 1, authority_length - colon - 1,
                        &out->port)) return 0;
    }

    const char *url_path = authority + authority_length;
    if (!url_path[0] || url_path[0] == '#')
        return copy_part(out->path, sizeof(out->path), "/", 1);
    if (url_path[0] == '?') {
        uint32_t query_length = 0;
        while (url_path[query_length] && url_path[query_length] != '#') ++query_length;
        if (query_length + 1 >= sizeof(out->path)) return 0;
        out->path[0] = '/';
        for (uint32_t index = 0; index < query_length; ++index)
            out->path[index + 1] = url_path[index];
        out->path[query_length + 1] = '\0';
        return valid_request_target(out->path);
    }
    uint32_t path_length = 0;
    while (url_path[path_length] && url_path[path_length] != '#') ++path_length;
    return copy_part(out->path, sizeof(out->path), url_path, path_length) &&
           valid_request_target(out->path);
}

static int serialize_url(const struct welovec_url *url,
                         char output[WELOVEC_URL_MAX_LENGTH]) {
    uint32_t length = 0;
    output[0] = '\0';
    const char *scheme = url->secure ? "https://" : "http://";
    const uint32_t scheme_length = url->secure ? 8u : 7u;
    for (uint32_t index = 0; index < scheme_length; ++index)
        if (!append_char(output, WELOVEC_URL_MAX_LENGTH, &length, scheme[index]))
            return 0;
    if (url->ipv6_literal &&
        !append_char(output, WELOVEC_URL_MAX_LENGTH, &length, '[')) return 0;
    const uint32_t host_length = bounded_length(url->host, sizeof(url->host));
    if (!host_length || host_length == sizeof(url->host) ||
        !append_part(output, WELOVEC_URL_MAX_LENGTH, &length,
                     url->host, host_length)) return 0;
    if (url->ipv6_literal &&
        !append_char(output, WELOVEC_URL_MAX_LENGTH, &length, ']')) return 0;
    const uint16_t default_port = url->secure ? 443u : 80u;
    if (!url->port) return 0;
    if (url->port != default_port) {
        char digits[5];
        uint32_t digit_count = 0;
        uint32_t port = url->port;
        do {
            digits[digit_count++] = (char)('0' + port % 10u);
            port /= 10u;
        } while (port && digit_count < sizeof(digits));
        if (!append_char(output, WELOVEC_URL_MAX_LENGTH, &length, ':')) return 0;
        while (digit_count)
            if (!append_char(output, WELOVEC_URL_MAX_LENGTH, &length,
                             digits[--digit_count])) return 0;
    }
    const uint32_t path_length = bounded_length(url->path, sizeof(url->path));
    if (!path_length || path_length == sizeof(url->path) ||
        !append_part(output, WELOVEC_URL_MAX_LENGTH, &length,
                     url->path, path_length)) return 0;
    return 1;
}

int welovec_url_format(const struct welovec_url *url,
                       char output[WELOVEC_URL_MAX_LENGTH]) {
    if (!url || !output) return 0;
    return serialize_url(url, output);
}

static int remove_last_path_segment(char *output, uint32_t *length) {
    if (!output || !length) return 0;
    uint32_t cursor = *length;
    while (cursor && output[cursor - 1] != '/') --cursor;
    *length = cursor ? cursor - 1 : 0;
    output[*length] = '\0';
    return 1;
}

/* RFC 3986 section 5.2.4, applied to an already absolute path. */
static int normalize_path(const char *input, uint32_t input_length,
                          char output[WELOVEC_URL_PATH_CAPACITY]) {
    uint32_t input_cursor = 0;
    uint32_t output_length = 0;
    output[0] = '\0';
    while (input_cursor < input_length) {
        const uint32_t remaining = input_length - input_cursor;
        const char *current = input + input_cursor;
        if (has_prefix(current, remaining, "../", 3)) {
            input_cursor += 3;
        } else if (has_prefix(current, remaining, "./", 2)) {
            input_cursor += 2;
        } else if (has_prefix(current, remaining, "/./", 3)) {
            input_cursor += 2;
        } else if (remaining == 2 && has_prefix(current, remaining, "/.", 2)) {
            input_cursor += 2;
            if (!output_length || output[output_length - 1] != '/')
                if (!append_char(output, WELOVEC_URL_PATH_CAPACITY,
                                 &output_length, '/')) return 0;
        } else if (has_prefix(current, remaining, "/../", 4)) {
            input_cursor += 3;
            if (!remove_last_path_segment(output, &output_length)) return 0;
        } else if (remaining == 3 && has_prefix(current, remaining, "/..", 3)) {
            input_cursor += 3;
            if (!remove_last_path_segment(output, &output_length)) return 0;
            if (!output_length || output[output_length - 1] != '/')
                if (!append_char(output, WELOVEC_URL_PATH_CAPACITY,
                                 &output_length, '/')) return 0;
        } else if ((remaining == 1 && current[0] == '.') ||
                   (remaining == 2 && current[0] == '.' && current[1] == '.')) {
            input_cursor = input_length;
        } else {
            uint32_t segment_length = 0;
            if (current[0] == '/') {
                segment_length = 1;
                while (input_cursor + segment_length < input_length &&
                       input[input_cursor + segment_length] != '/')
                    ++segment_length;
            } else {
                while (input_cursor + segment_length < input_length &&
                       input[input_cursor + segment_length] != '/')
                    ++segment_length;
            }
            if (!segment_length ||
                !append_part(output, WELOVEC_URL_PATH_CAPACITY, &output_length,
                             current, segment_length)) return 0;
            input_cursor += segment_length;
        }
    }
    if (!output_length &&
        !append_char(output, WELOVEC_URL_PATH_CAPACITY, &output_length, '/'))
        return 0;
    return 1;
}

static int path_without_query(const char *path, uint32_t length,
                              char output[WELOVEC_URL_PATH_CAPACITY],
                              uint32_t *output_length,
                              uint32_t *query_offset) {
    uint32_t path_length = 0;
    while (path_length < length && path[path_length] != '?') ++path_length;
    if (!copy_part(output, WELOVEC_URL_PATH_CAPACITY, path, path_length)) return 0;
    *output_length = path_length;
    *query_offset = path_length;
    return 1;
}

int welovec_url_resolve(const struct welovec_url *base, const char *reference,
                        struct welovec_url *out) {
    if (!base || !reference || !out) return 0;
    const uint32_t reference_length =
        bounded_length(reference, WELOVEC_URL_MAX_LENGTH);
    if (reference_length == WELOVEC_URL_MAX_LENGTH) return 0;

    char absolute[WELOVEC_URL_MAX_LENGTH];
    uint32_t absolute_length = 0;
    if (!serialize_url(base, absolute)) return 0;
    struct welovec_url parsed_base;
    if (!welovec_url_parse(absolute, &parsed_base)) return 0;

    uint32_t fragment_offset = reference_length;
    for (uint32_t index = 0; index < reference_length; ++index) {
        if (reference[index] == '#') {
            fragment_offset = index;
            break;
        }
    }
    uint32_t scheme_end = fragment_offset;
    for (uint32_t index = 0; index < fragment_offset; ++index) {
        if (reference[index] == ':') {
            scheme_end = index;
            break;
        }
        if (reference[index] == '/' || reference[index] == '?') break;
    }
    if (scheme_end < fragment_offset) return welovec_url_parse(reference, out);

    if (fragment_offset >= 2 && reference[0] == '/' && reference[1] == '/') {
        absolute_length = 0;
        const char *scheme = parsed_base.secure ? "https:" : "http:";
        const uint32_t scheme_length = parsed_base.secure ? 6u : 5u;
        for (uint32_t index = 0; index < scheme_length; ++index)
            if (!append_char(absolute, sizeof(absolute), &absolute_length,
                             scheme[index])) return 0;
        if (!append_part(absolute, sizeof(absolute), &absolute_length,
                         reference, fragment_offset)) return 0;
        return welovec_url_parse(absolute, out);
    }

    char base_path[WELOVEC_URL_PATH_CAPACITY];
    char normalized_base_path[WELOVEC_URL_PATH_CAPACITY];
    uint32_t base_path_length = 0;
    uint32_t base_query_offset = 0;
    const uint32_t parsed_base_path_length =
        bounded_length(parsed_base.path, sizeof(parsed_base.path));
    if (!parsed_base_path_length ||
        parsed_base_path_length == sizeof(parsed_base.path) ||
        !path_without_query(parsed_base.path, parsed_base_path_length,
                            base_path, &base_path_length, &base_query_offset) ||
        !normalize_path(base_path, base_path_length, normalized_base_path)) return 0;
    const uint32_t normalized_base_length = bounded_length(
        normalized_base_path, sizeof(normalized_base_path));
    if (normalized_base_length == sizeof(normalized_base_path)) return 0;

    uint32_t reference_path_length = 0;
    while (reference_path_length < fragment_offset &&
           reference[reference_path_length] != '?') ++reference_path_length;
    const int has_reference_query = reference_path_length < fragment_offset;
    const uint32_t query_end = fragment_offset;
    uint32_t query_start = reference_path_length;
    char merged_path[WELOVEC_URL_MAX_LENGTH];
    uint32_t merged_length = 0;
    merged_path[0] = '\0';

    if (!reference_path_length) {
        if (!append_part(merged_path, sizeof(merged_path), &merged_length,
                         normalized_base_path, normalized_base_length)) return 0;
    } else if (reference[0] == '/') {
        if (!append_part(merged_path, sizeof(merged_path), &merged_length,
                         reference, reference_path_length)) return 0;
    } else {
        uint32_t directory_length = normalized_base_length;
        while (directory_length && normalized_base_path[directory_length - 1] != '/')
            --directory_length;
        if (!append_part(merged_path, sizeof(merged_path), &merged_length,
                         normalized_base_path, directory_length) ||
            !append_part(merged_path, sizeof(merged_path), &merged_length,
                         reference, reference_path_length)) return 0;
    }

    char resolved_path[WELOVEC_URL_PATH_CAPACITY];
    if (!normalize_path(merged_path, merged_length, resolved_path)) return 0;
    uint32_t resolved_length = bounded_length(resolved_path, sizeof(resolved_path));
    if (resolved_length == sizeof(resolved_path)) return 0;

    if (has_reference_query) {
        if (!append_part(resolved_path, sizeof(resolved_path), &resolved_length,
                         reference + query_start, query_end - query_start)) return 0;
    } else if (!reference_path_length && base_query_offset < parsed_base_path_length) {
        if (!append_part(resolved_path, sizeof(resolved_path), &resolved_length,
                         parsed_base.path + base_query_offset,
                         parsed_base_path_length - base_query_offset)) return 0;
    }
    if (!valid_request_target(resolved_path)) return 0;

    absolute_length = 0;
    const char *scheme = parsed_base.secure ? "https://" : "http://";
    const uint32_t scheme_length = parsed_base.secure ? 8u : 7u;
    for (uint32_t index = 0; index < scheme_length; ++index)
        if (!append_char(absolute, sizeof(absolute), &absolute_length,
                         scheme[index])) return 0;
    if (parsed_base.ipv6_literal &&
        !append_char(absolute, sizeof(absolute), &absolute_length, '[')) return 0;
    const uint32_t host_length = bounded_length(parsed_base.host,
                                                sizeof(parsed_base.host));
    if (!host_length || host_length == sizeof(parsed_base.host) ||
        !append_part(absolute, sizeof(absolute), &absolute_length,
                     parsed_base.host, host_length)) return 0;
    if (parsed_base.ipv6_literal &&
        !append_char(absolute, sizeof(absolute), &absolute_length, ']')) return 0;
    const uint16_t default_port = parsed_base.secure ? 443u : 80u;
    if (parsed_base.port != default_port) {
        char digits[5];
        uint32_t digit_count = 0;
        uint32_t port = parsed_base.port;
        do {
            digits[digit_count++] = (char)('0' + port % 10u);
            port /= 10u;
        } while (port && digit_count < sizeof(digits));
        if (!append_char(absolute, sizeof(absolute), &absolute_length, ':')) return 0;
        while (digit_count)
            if (!append_char(absolute, sizeof(absolute), &absolute_length,
                             digits[--digit_count])) return 0;
    }
    if (!append_part(absolute, sizeof(absolute), &absolute_length,
                     resolved_path, resolved_length)) return 0;
    return welovec_url_parse(absolute, out);
}

int welovec_url_same_origin(const struct welovec_url *left,
                            const struct welovec_url *right) {
    if (!left || !right || left->secure != right->secure ||
        left->port != right->port || left->ipv6_literal != right->ipv6_literal)
        return 0;
    if (left->ipv6_literal) {
        uint8_t left_address[16], right_address[16];
        if (!welovec_url_parse_ipv6(left->host, left_address) ||
            !welovec_url_parse_ipv6(right->host, right_address)) return 0;
        for (uint32_t index = 0; index < 16; ++index)
            if (left_address[index] != right_address[index]) return 0;
        return 1;
    }
    const uint32_t left_length = bounded_length(left->host, sizeof(left->host));
    const uint32_t right_length = bounded_length(right->host, sizeof(right->host));
    if (!left_length || left_length != right_length ||
        left_length == sizeof(left->host) || right_length == sizeof(right->host))
        return 0;
    for (uint32_t index = 0; index < left_length; ++index)
        if (ascii_lower(left->host[index]) != ascii_lower(right->host[index]))
            return 0;
    return 1;
}
