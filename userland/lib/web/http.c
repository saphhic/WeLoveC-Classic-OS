/* Written by [@saphhic](https://github.com/saphhic)
 * Date: October 31st 2026
 * Description: Implements bounded HTTP/1.x GET over TCP and TLS.
 */

#include "http.h"

#include <stdint.h>
#include <string.h>

#include "user/network.h"
#include "user/syscall.h"
#include "../tls/tls_client.h"

#define HTTP_READ_CAPACITY WELOVEC_TCP_MAX_IO
#define HTTP_OPERATION_TIMEOUT_TICKS 3000u
#define HTTP_CHUNK_LINE_MAX 128u

struct http_stream {
    int socket;
    struct welovec_tls_session *tls;
    uint8_t buffer[HTTP_READ_CAPACITY];
    size_t offset;
    size_t length;
    uint64_t last_progress;
    int error;
};

struct response_headers {
    uint16_t status;
    uint32_t content_length;
    uint8_t has_content_length;
    uint8_t chunked;
    uint8_t has_transfer_encoding;
    uint8_t has_location;
    char location[WELOVEC_URL_MAX_LENGTH];
    char content_type[128];
};

static int is_redirect(uint16_t status);

static int connect_error_from_errno(int64_t result) {
    switch (result) {
    case -11: return WELOVEC_HTTP_CONNECT_ENTROPY_ERROR; /* EAGAIN */
    case -5: return WELOVEC_HTTP_CONNECT_SEND_ERROR; /* EIO */
    case -24: case -99:
        return WELOVEC_HTTP_CONNECT_RESOURCE_ERROR; /* EMFILE/EADDRNOTAVAIL */
    case -101: case -113:
        return WELOVEC_HTTP_CONNECT_NETWORK_ERROR; /* ENETUNREACH/EHOSTUNREACH */
    case -110: return WELOVEC_HTTP_CONNECT_TIMEOUT; /* ETIMEDOUT */
    case -111: return WELOVEC_HTTP_CONNECT_REFUSED; /* ECONNREFUSED */
    default: return WELOVEC_HTTP_CONNECT_ERROR;
    }
}

static char lower_ascii(char value) {
    return value >= 'A' && value <= 'Z' ? (char)(value - 'A' + 'a') : value;
}

static int span_equal_ci(const char *left, size_t left_length,
                         const char *right) {
    size_t index = 0;
    while (right[index]) {
        if (index >= left_length || lower_ascii(left[index]) != right[index])
            return 0;
        ++index;
    }
    return index == left_length;
}

static size_t bounded_length(const char *text, size_t capacity) {
    size_t length = 0;
    if (!text) return capacity;
    while (length < capacity && text[length]) ++length;
    return length;
}

static int wait_for_data(struct http_stream *stream) {
    for (;;) {
        int64_t received;
        if (stream->tls) {
            received = welovec_tls_receive(stream->tls, stream->buffer,
                                           sizeof(stream->buffer));
        } else {
            received = welovec_tcp_receive(stream->socket, stream->buffer,
                                            sizeof(stream->buffer));
        }
        if (received < 0) {
            stream->error = WELOVEC_HTTP_RECEIVE_ERROR;
            return 0;
        }
        if (received > 0) {
            stream->offset = 0;
            stream->length = (size_t)received;
            stream->last_progress = welovec_getticks();
            return 1;
        }
        if (welovec_getticks() - stream->last_progress >=
            HTTP_OPERATION_TIMEOUT_TICKS) {
            stream->error = WELOVEC_HTTP_TIMEOUT;
            return 0;
        }
        (void)welovec_sleep(1);
    }
}

static int stream_read(struct http_stream *stream, uint8_t *output,
                       size_t capacity, size_t *read_count) {
    if (!stream || !read_count || (!output && capacity)) return 0;
    *read_count = 0;
    if (!capacity) return 1;
    if (stream->offset == stream->length && !wait_for_data(stream)) return 0;
    size_t available = stream->length - stream->offset;
    if (available > capacity) available = capacity;
    memcpy(output, stream->buffer + stream->offset, available);
    stream->offset += available;
    *read_count = available;
    return 1;
}

static int stream_read_exact(struct http_stream *stream, uint8_t *output,
                             size_t size) {
    size_t offset = 0;
    while (offset < size) {
        size_t count = 0;
        if (!stream_read(stream, output + offset, size - offset, &count))
            return 0;
        if (!count) return 0;
        offset += count;
    }
    return 1;
}

static int stream_read_line(struct http_stream *stream, char *line,
                            size_t capacity, size_t *line_length) {
    size_t used = 0;
    if (!line || capacity < 2 || !line_length) return 0;
    for (;;) {
        uint8_t character;
        if (!stream_read_exact(stream, &character, 1)) return 0;
        if (used + 1 >= capacity) return 0;
        line[used++] = (char)character;
        if (used >= 2 && line[used - 2] == '\r' && line[used - 1] == '\n') {
            line[used - 2] = '\0';
            *line_length = used - 2;
            return 1;
        }
    }
}

static int parse_decimal(const char *text, size_t length, uint32_t *value) {
    uint32_t parsed = 0;
    if (!length || !value) return 0;
    for (size_t index = 0; index < length; ++index) {
        if (text[index] < '0' || text[index] > '9') return 0;
        const uint32_t digit = (uint32_t)(text[index] - '0');
        if (parsed > (UINT32_MAX - digit) / 10u) return 0;
        parsed = parsed * 10u + digit;
    }
    *value = parsed;
    return 1;
}

static int copy_header_value(char *output, size_t capacity,
                             const char *value, size_t length) {
    while (length && (*value == ' ' || *value == '\t')) {
        ++value;
        --length;
    }
    while (length && (value[length - 1] == ' ' || value[length - 1] == '\t'))
        --length;
    if (length >= capacity) return 0;
    memcpy(output, value, length);
    output[length] = '\0';
    return 1;
}

static int parse_status_line(const char *line, size_t length, uint16_t *status) {
    size_t cursor = 0;
    if (!status || length < 12 ||
        (memcmp(line, "HTTP/1.0 ", 9) != 0 &&
         memcmp(line, "HTTP/1.1 ", 9) != 0)) return 0;
    cursor = 9;
    if (line[cursor] < '0' || line[cursor] > '9' ||
        line[cursor + 1] < '0' || line[cursor + 1] > '9' ||
        line[cursor + 2] < '0' || line[cursor + 2] > '9' ||
        (cursor + 3 < length && line[cursor + 3] != ' ')) return 0;
    *status = (uint16_t)((line[cursor] - '0') * 100 +
                         (line[cursor + 1] - '0') * 10 +
                         line[cursor + 2] - '0');
    return *status >= 100;
}

static int parse_headers(char *block, size_t length,
                         struct response_headers *headers) {
    if (!block || !headers || length < 4) return 0;
    memset(headers, 0, sizeof(*headers));
    size_t line_end = 0;
    while (line_end + 1 < length &&
           !(block[line_end] == '\r' && block[line_end + 1] == '\n'))
        ++line_end;
    if (line_end + 1 >= length ||
        !parse_status_line(block, line_end, &headers->status)) return 0;

    size_t cursor = line_end + 2;
    while (cursor + 1 < length) {
        line_end = cursor;
        while (line_end + 1 < length &&
               !(block[line_end] == '\r' && block[line_end + 1] == '\n'))
            ++line_end;
        if (line_end + 1 >= length) return 0;
        if (line_end == cursor)
            return !(headers->has_content_length &&
                     headers->has_transfer_encoding);

        size_t colon = cursor;
        while (colon < line_end && block[colon] != ':') ++colon;
        if (colon == cursor || colon == line_end) return 0;
        const char *value = block + colon + 1;
        size_t value_length = line_end - colon - 1;
        while (value_length && (*value == ' ' || *value == '\t')) {
            ++value;
            --value_length;
        }
        while (value_length &&
               (value[value_length - 1] == ' ' || value[value_length - 1] == '\t'))
            --value_length;

        const size_t name_length = colon - cursor;
        if (span_equal_ci(block + cursor, name_length, "content-length")) {
            uint32_t content_length;
            if (headers->has_content_length ||
                !parse_decimal(value, value_length, &content_length)) return 0;
            headers->content_length = content_length;
            headers->has_content_length = 1;
        } else if (span_equal_ci(block + cursor, name_length,
                                 "transfer-encoding")) {
            if (headers->has_transfer_encoding ||
                !span_equal_ci(value, value_length, "chunked")) return 0;
            headers->has_transfer_encoding = 1;
            headers->chunked = 1;
        } else if (span_equal_ci(block + cursor, name_length, "location")) {
            if (headers->has_location ||
                !copy_header_value(headers->location,
                                  sizeof(headers->location), value,
                                  value_length)) return 0;
            headers->has_location = 1;
        } else if (span_equal_ci(block + cursor, name_length, "content-type")) {
            if (!copy_header_value(headers->content_type,
                                  sizeof(headers->content_type), value,
                                  value_length)) return 0;
        }
        cursor = line_end + 2;
    }
    return 0;
}

static int read_headers(struct http_stream *stream,
                       struct response_headers *headers) {
    char block[WELOVEC_HTTP_HEADER_MAX + 1u];
    size_t used = 0;
    for (;;) {
        uint8_t character;
        if (used == WELOVEC_HTTP_HEADER_MAX ||
            !stream_read_exact(stream, &character, 1)) return 0;
        block[used++] = (char)character;
        if (used >= 4 && block[used - 4] == '\r' &&
            block[used - 3] == '\n' && block[used - 2] == '\r' &&
            block[used - 1] == '\n') {
            block[used] = '\0';
            return parse_headers(block, used, headers);
        }
    }
}

static int parse_chunk_size(const char *line, size_t length,
                            uint32_t *chunk_size) {
    uint32_t parsed = 0;
    size_t index = 0;
    size_t digits = 0;
    if (!line || !chunk_size) return 0;
    while (index < length && line[index] != ';') {
        const char character = line[index++];
        uint32_t digit;
        if (character >= '0' && character <= '9') digit = (uint32_t)(character - '0');
        else if (character >= 'a' && character <= 'f') digit = (uint32_t)(character - 'a' + 10);
        else if (character >= 'A' && character <= 'F') digit = (uint32_t)(character - 'A' + 10);
        else return 0;
        if (parsed > (UINT32_MAX - digit) / 16u) return 0;
        parsed = parsed * 16u + digit;
        ++digits;
    }
    if (!digits) return 0;
    *chunk_size = parsed;
    return 1;
}

static int read_chunked_body(struct http_stream *stream, uint8_t *body,
                             size_t capacity, size_t *body_size) {
    size_t used = 0;
    char line[HTTP_CHUNK_LINE_MAX];
    for (;;) {
        size_t line_length = 0;
        uint32_t chunk_size;
        if (!stream_read_line(stream, line, sizeof(line), &line_length) ||
            !parse_chunk_size(line, line_length, &chunk_size)) return 0;
        if (!chunk_size) {
            size_t trailer_bytes = 0;
            do {
                if (!stream_read_line(stream, line, sizeof(line), &line_length))
                    return 0;
                trailer_bytes += line_length + 2;
                if (trailer_bytes > WELOVEC_HTTP_HEADER_MAX) return 0;
            } while (line_length);
            *body_size = used;
            return 1;
        }
        if (chunk_size > capacity - used) return -1;
        if (!stream_read_exact(stream, body + used, chunk_size)) return 0;
        used += chunk_size;
        uint8_t ending[2];
        if (!stream_read_exact(stream, ending, sizeof(ending)) ||
            ending[0] != '\r' || ending[1] != '\n') return 0;
    }
}

static int read_response_body(struct http_stream *stream,
                              const struct response_headers *headers,
                              uint8_t *body, size_t capacity,
                              size_t *body_size) {
    *body_size = 0;
    if (headers->status == 204 || headers->status == 304 ||
        (headers->status >= 100 && headers->status < 200)) return 1;
    if (headers->chunked && headers->has_content_length) return 0;
    if (headers->chunked) {
        const int result = read_chunked_body(stream, body, capacity, body_size);
        return result;
    }
    if (!headers->has_content_length) return -2;
    if (headers->content_length > capacity) return -1;
    if (headers->content_length &&
        !stream_read_exact(stream, body, headers->content_length)) return 0;
    *body_size = headers->content_length;
    return 1;
}

static int connect_url(const struct welovec_url *url, int *socket_out) {
    uint8_t address4[4];
    uint8_t address6[16];
    int64_t socket;
    if (url->ipv6_literal) {
        if (!welovec_url_parse_ipv6(url->host, address6))
            return WELOVEC_HTTP_INVALID_URL;
        socket = welovec_tcp_connect_ipv6(address6, url->port);
        if (socket <= 0 || socket > INT32_MAX)
            return connect_error_from_errno(socket);
        *socket_out = (int)socket;
        return WELOVEC_HTTP_OK;
    }

    int resolved = 0;
    int connect_failed = 0;
    int64_t connect_error = 0;
    if (welovec_dns_lookup_ipv4(url->host, address4) == 0) {
        resolved = 1;
        socket = welovec_tcp_connect_ipv4(address4, url->port);
        if (socket > 0 && socket <= INT32_MAX) {
            *socket_out = (int)socket;
            return WELOVEC_HTTP_OK;
        }
        connect_failed = 1;
        connect_error = socket;
    }
    if (welovec_dns_lookup_ipv6(url->host, address6) == 0) {
        resolved = 1;
        socket = welovec_tcp_connect_ipv6(address6, url->port);
        if (socket > 0 && socket <= INT32_MAX) {
            *socket_out = (int)socket;
            return WELOVEC_HTTP_OK;
        }
        connect_failed = 1;
        if (!connect_error) connect_error = socket;
    }
    if (connect_failed) return connect_error_from_errno(connect_error);
    return resolved ? WELOVEC_HTTP_CONNECT_ERROR : WELOVEC_HTTP_DNS_ERROR;
}

static int append_text(char *output, size_t capacity, size_t *length,
                       const char *text, size_t text_length) {
    if (!output || !length || !text || *length > capacity ||
        text_length >= capacity - *length) return 0;
    memcpy(output + *length, text, text_length);
    *length += text_length;
    output[*length] = '\0';
    return 1;
}

static int build_request(const struct welovec_url *url,
                         char request[WELOVEC_URL_MAX_LENGTH + 512u],
                         size_t *request_length) {
    char host_header[WELOVEC_URL_HOST_CAPACITY + 16u];
    size_t host_length = 0;
    host_header[0] = '\0';
    if (url->ipv6_literal && !append_text(host_header, sizeof(host_header),
                                          &host_length, "[", 1)) return 0;
    const size_t host_size = bounded_length(url->host, sizeof(url->host));
    if (!host_size || host_size == sizeof(url->host) ||
        !append_text(host_header, sizeof(host_header), &host_length,
                     url->host, host_size)) return 0;
    if (url->ipv6_literal && !append_text(host_header, sizeof(host_header),
                                          &host_length, "]", 1)) return 0;
    const uint16_t default_port = url->secure ? 443u : 80u;
    if (url->port != default_port) {
        char digits[6];
        size_t used = 0;
        uint32_t port = url->port;
        do {
            digits[used++] = (char)('0' + port % 10u);
            port /= 10u;
        } while (port && used < sizeof(digits));
        if (!append_text(host_header, sizeof(host_header), &host_length,
                         ":", 1)) return 0;
        while (used)
            if (!append_text(host_header, sizeof(host_header), &host_length,
                             &digits[--used], 1)) return 0;
    }

    size_t length = 0;
    request[0] = '\0';
    const char *prefix = "GET ";
    const size_t path_length = bounded_length(url->path, sizeof(url->path));
    if (!path_length || path_length == sizeof(url->path) ||
        !append_text(request, WELOVEC_URL_MAX_LENGTH + 512u, &length,
                     prefix, 4) ||
        !append_text(request, WELOVEC_URL_MAX_LENGTH + 512u, &length,
                     url->path, path_length) ||
        !append_text(request, WELOVEC_URL_MAX_LENGTH + 512u, &length,
                     " HTTP/1.1\r\nHost: ", 17) ||
        !append_text(request, WELOVEC_URL_MAX_LENGTH + 512u, &length,
                     host_header, host_length) ||
        !append_text(request, WELOVEC_URL_MAX_LENGTH + 512u, &length,
                     "\r\nUser-Agent: WeLoveC-Classic/0.1\r\n"
                     "Accept: text/html, text/plain;q=0.9, */*;q=0.1\r\n"
                     "Accept-Encoding: identity\r\nConnection: close\r\n\r\n",
                     sizeof("\r\nUser-Agent: WeLoveC-Classic/0.1\r\n"
                            "Accept: text/html, text/plain;q=0.9, */*;q=0.1\r\n"
                            "Accept-Encoding: identity\r\nConnection: close\r\n\r\n") - 1))
        return 0;
    *request_length = length;
    return 1;
}

static int request_once(const struct welovec_url *url, uint8_t *body,
                        size_t body_capacity,
                        struct response_headers *headers, size_t *body_size,
                        int *tls_error) {
    int socket = 0;
    struct welovec_tls_session *tls = 0;
    int result = WELOVEC_HTTP_NETWORK_ERROR;
    char request[WELOVEC_URL_MAX_LENGTH + 512u];
    size_t request_length = 0;
    const int connection_result = connect_url(url, &socket);
    if (connection_result != WELOVEC_HTTP_OK) return connection_result;
    if (url->secure) {
        int local_error = 0;
        tls = welovec_tls_connect(socket, url->host, &local_error);
        if (!tls) {
            if (tls_error) *tls_error = local_error;
            result = WELOVEC_HTTP_TLS_ERROR;
            goto cleanup;
        }
    }
    if (!build_request(url, request, &request_length)) {
        result = WELOVEC_HTTP_INVALID_URL;
        goto cleanup;
    }
    if (tls) {
        const int sent = welovec_tls_send(tls, request, request_length);
        if (sent < 0 || (size_t)sent != request_length) {
            result = WELOVEC_HTTP_SEND_ERROR;
            goto cleanup;
        }
    } else {
        size_t sent = 0;
        while (sent < request_length) {
            const int64_t count = welovec_tcp_send(
                socket, request + sent, request_length - sent);
            if (count <= 0) {
                result = WELOVEC_HTTP_SEND_ERROR;
                goto cleanup;
            }
            sent += (size_t)count;
        }
    }

    struct http_stream stream;
    memset(&stream, 0, sizeof(stream));
    stream.socket = socket;
    stream.tls = tls;
    stream.last_progress = welovec_getticks();
    if (!read_headers(&stream, headers)) {
        result = stream.error ? stream.error : WELOVEC_HTTP_BAD_RESPONSE;
        goto cleanup;
    }
    if (is_redirect(headers->status)) {
        result = WELOVEC_HTTP_OK;
        *body_size = 0;
        goto cleanup;
    }
    const int body_result = read_response_body(&stream, headers, body,
                                               body_capacity, body_size);
    if (body_result == -1) result = WELOVEC_HTTP_BODY_TOO_LARGE;
    else if (body_result == -2) result = WELOVEC_HTTP_UNSUPPORTED_RESPONSE;
    else if (!body_result) {
        result = stream.error ? stream.error : WELOVEC_HTTP_BAD_RESPONSE;
    } else {
        result = WELOVEC_HTTP_OK;
    }

cleanup:
    if (tls) welovec_tls_close(tls);
    (void)welovec_tcp_close(socket);
    return result;
}

static int is_redirect(uint16_t status) {
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

int welovec_http_get(const char *url_text, uint8_t *body,
                     size_t body_capacity,
                     struct welovec_http_response *response, int *tls_error) {
    char current[WELOVEC_URL_MAX_LENGTH];
    struct welovec_url current_url;
    if (tls_error) *tls_error = 0;
    if (!url_text || !body || !body_capacity || !response ||
        !welovec_url_parse(url_text, &current_url) ||
        !welovec_url_format(&current_url, current))
        return WELOVEC_HTTP_INVALID_URL;

    memset(response, 0, sizeof(*response));
    for (uint32_t redirect_count = 0; ; ++redirect_count) {
        struct response_headers headers;
        size_t body_size = 0;
        const int result = request_once(&current_url, body, body_capacity,
                                        &headers, &body_size, tls_error);
        if (result != WELOVEC_HTTP_OK) return result;
        if (is_redirect(headers.status)) {
            if (!headers.has_location || redirect_count >= WELOVEC_HTTP_REDIRECT_MAX)
                return WELOVEC_HTTP_REDIRECT_ERROR;
            struct welovec_url next_url;
            char next_text[WELOVEC_URL_MAX_LENGTH];
            if (!welovec_url_resolve(&current_url, headers.location, &next_url) ||
                (current_url.secure && !next_url.secure) ||
                !welovec_url_format(&next_url, next_text))
                return WELOVEC_HTTP_REDIRECT_ERROR;
            current_url = next_url;
            memcpy(current, next_text, strlen(next_text) + 1u);
            continue;
        }
        response->status = headers.status;
        response->body_size = body_size;
        memcpy(response->final_url, current, strlen(current) + 1u);
        memcpy(response->content_type, headers.content_type,
               sizeof(response->content_type));
        return WELOVEC_HTTP_OK;
    }
}

const char *welovec_http_error_string(int error) {
    switch (error) {
    case WELOVEC_HTTP_INVALID_URL: return "invalid or unsupported URL";
    case WELOVEC_HTTP_NETWORK_ERROR: return "could not connect to or read from the network";
    case WELOVEC_HTTP_TLS_ERROR: return "TLS or certificate validation failed";
    case WELOVEC_HTTP_BAD_RESPONSE: return "malformed HTTP response";
    case WELOVEC_HTTP_BODY_TOO_LARGE: return "page exceeds the memory limit";
    case WELOVEC_HTTP_UNSUPPORTED_RESPONSE: return "response has unsupported length or encoding";
    case WELOVEC_HTTP_REDIRECT_ERROR: return "redirect is missing, excessive, or unsafe";
    case WELOVEC_HTTP_TIMEOUT: return "network request timed out";
    case WELOVEC_HTTP_DNS_ERROR: return "DNS could not resolve the server name";
    case WELOVEC_HTTP_CONNECT_ERROR: return "could not open a TCP connection to the server";
    case WELOVEC_HTTP_SEND_ERROR: return "TCP connection failed while sending the request";
    case WELOVEC_HTTP_RECEIVE_ERROR: return "TCP connection failed while receiving the response";
    case WELOVEC_HTTP_CONNECT_TIMEOUT: return "server did not respond to the start of the TCP connection";
    case WELOVEC_HTTP_CONNECT_REFUSED: return "server refused the TCP connection; check the port and protocol";
    case WELOVEC_HTTP_CONNECT_NETWORK_ERROR: return "no route or network response from the server";
    case WELOVEC_HTTP_CONNECT_ENTROPY_ERROR: return "kernel has no secure random source for TCP";
    case WELOVEC_HTTP_CONNECT_RESOURCE_ERROR: return "kernel could not allocate resources for the TCP connection";
    case WELOVEC_HTTP_CONNECT_SEND_ERROR: return "kernel could not send the TCP connection handshake";
    default: return "unknown HTTP error";
    }
}
