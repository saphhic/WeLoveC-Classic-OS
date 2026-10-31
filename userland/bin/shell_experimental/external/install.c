// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Downloads and installs executables or saves downloaded files.

#include "cmd_common.h"
#include "../include/command_config.h"
#include "lib/tls/tls_client.h"
#include "lib/web/url.h"
#include "user/network.h"
#include "user/security.h"

#define INSTALL_RESPONSE_MAX_CAPACITY (UINT64_C(500) * 1024 * 1024)
#define INSTALL_RESPONSE_FALLBACK_CAPACITY (UINT64_C(250) * 1024 * 1024)
#define INSTALL_HEADER_LIMIT 8192u
#define INSTALL_MAX_BYTES_PER_SECOND UINT64_C(500000000)
#define INSTALL_BYTES_PER_TICK (INSTALL_MAX_BYTES_PER_SECOND / 100u)
#define INSTALL_FS_WRITE_CHUNK (16u * 1024u * 1024u)

static uint8_t *response_data;
static uint64_t response_capacity;

static char ascii_lower(char value) {
    return value >= 'A' && value <= 'Z' ? (char)(value - 'A' + 'a') : value;
}

static int equal_case_insensitive(const char *left, const char *right,
                                  uint32_t length) {
    for (uint32_t index = 0; index < length; ++index)
        if (ascii_lower(left[index]) != ascii_lower(right[index])) return 0;
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

static int64_t transport_send(int socket, struct welovec_tls_session *tls,
                              const void *data, uint64_t size) {
    return tls ? welovec_tls_send(tls, data, (size_t)size)
               : welovec_tcp_send(socket, data, size);
}

static int64_t transport_receive(int socket, struct welovec_tls_session *tls,
                                 void *data, uint64_t capacity) {
    return tls ? welovec_tls_receive(tls, data, (size_t)capacity)
               : welovec_tcp_receive(socket, data, capacity);
}

static void close_transport(int socket, struct welovec_tls_session *tls) {
    welovec_tls_close(tls);
    (void)welovec_tcp_close(socket);
}

static int find_header_end(uint32_t size, uint32_t *body_start) {
    if (size < 4) return 0;
    for (uint32_t index = 0; index + 3 < size && index < INSTALL_HEADER_LIMIT; ++index) {
        if (response_data[index] == '\r' && response_data[index + 1] == '\n' &&
            response_data[index + 2] == '\r' && response_data[index + 3] == '\n') {
            *body_start = index + 4;
            return 1;
        }
    }
    return 0;
}

static int parse_response_headers(uint32_t body_start, uint32_t *content_length,
                                  int *has_length, int *chunked) {
    uint32_t line_end = 0;
    while (line_end + 1 < body_start &&
           !(response_data[line_end] == '\r' && response_data[line_end + 1] == '\n'))
        ++line_end;
    if (line_end < 12 || response_data[0] != 'H' || response_data[1] != 'T' ||
        response_data[2] != 'T' || response_data[3] != 'P' ||
        response_data[9] != '2' || response_data[10] != '0' ||
        response_data[11] != '0') return 0;

    uint32_t cursor = line_end + 2;
    while (cursor + 1 < body_start) {
        uint32_t end = cursor;
        while (end + 1 < body_start &&
               !(response_data[end] == '\r' && response_data[end + 1] == '\n'))
            ++end;
        const uint32_t header_length = end - cursor;
        const char *header = (const char *)response_data + cursor;
        if (header_length >= 15 && equal_case_insensitive(header, "content-length:", 15)) {
            if (*has_length) return 0;
            uint32_t value_cursor = cursor + 15;
            while (value_cursor < end && response_data[value_cursor] == ' ') ++value_cursor;
            uint32_t value = 0;
            if (value_cursor == end) return 0;
            while (value_cursor < end) {
                const char digit = (char)response_data[value_cursor++];
                if (digit < '0' || digit > '9') return 0;
                if (value > (response_capacity - 1) / 10) return 0;
                value = value * 10 + (uint32_t)(digit - '0');
                if (value >= response_capacity) return 0;
            }
            *content_length = value;
            *has_length = 1;
        } else if (header_length >= 18 &&
                   equal_case_insensitive(header, "transfer-encoding:", 18)) {
            uint32_t value_cursor = cursor + 18;
            while (value_cursor < end && response_data[value_cursor] == ' ') ++value_cursor;
            if (end - value_cursor >= 7 &&
                equal_case_insensitive((const char *)response_data + value_cursor,
                                       "chunked", 7)) *chunked = 1;
        }
        cursor = end + 2;
    }
    return 1;
}

static int make_default_destination(const char *url_path,
                                   char destination[WELOVEC_USER_PATH_MAX]) {
    uint32_t end = 0;
    while (url_path[end] && url_path[end] != '?' && url_path[end] != '#') ++end;
    while (end && url_path[end - 1] == '/') --end;
    uint32_t basename = end;
    while (basename && url_path[basename - 1] != '/') --basename;
    if (basename == end) return 0;
    if (end - basename == 1 && url_path[basename] == '.') return 0;
    if (end - basename == 2 && url_path[basename] == '.' &&
        url_path[basename + 1] == '.') return 0;
    for (uint32_t index = basename; index < end; ++index)
        if (url_path[index] == '\\' || url_path[index] == ':' ||
            url_path[index] < 0x20) return 0;

    struct welovec_user_status status;
    if (welovec_get_user_status(&status) == 0 && status.authenticated && status.name[0]) {
        static const char command_bin[] = "/userspace/bin/shell/bin/";
        char bin[WELOVEC_USER_PATH_MAX];
        uint32_t cursor = 0;
        for (uint32_t index = 0; command_bin[index]; ++index)
            bin[cursor++] = command_bin[index];
        for (uint32_t index = basename; index < end; ++index) {
            if (cursor + 1 >= sizeof(bin)) return 0;
            bin[cursor++] = url_path[index];
        }
        bin[cursor] = '\0';
        if (!copy_part(destination, WELOVEC_USER_PATH_MAX, bin, cursor)) return 0;
        return 1;
    }

    const char prefix[] = "/userspace/bin/shell/";
    uint32_t cursor = 0;
    while (prefix[cursor]) destination[cursor] = prefix[cursor], ++cursor;
    for (uint32_t index = basename; index < end; ++index) {
        if (cursor + 1 >= WELOVEC_USER_PATH_MAX) return 0;
        destination[cursor++] = url_path[index];
    }
    destination[cursor] = '\0';
    return 1;
}

static int make_default_file_destination(
    const char *url_path, char destination[WELOVEC_USER_PATH_MAX]) {
    uint32_t end = 0;
    while (url_path[end] && url_path[end] != '?' && url_path[end] != '#') ++end;
    while (end && url_path[end - 1] == '/') --end;
    uint32_t basename = end;
    while (basename && url_path[basename - 1] != '/') --basename;
    if (basename == end || (end - basename == 1 && url_path[basename] == '.') ||
        (end - basename == 2 && url_path[basename] == '.' &&
         url_path[basename + 1] == '.')) return 0;
    for (uint32_t index = basename; index < end; ++index)
        if (url_path[index] == '\\' || url_path[index] == ':' ||
            url_path[index] < 0x20) return 0;

    struct welovec_user_status status;
    const int authenticated = welovec_get_user_status(&status) == 0 &&
                             status.authenticated && status.name[0];
    uint32_t cursor = 0;
    if (authenticated) {
        static const char home_prefix[] = "/home/";
        for (uint32_t index = 0; home_prefix[index]; ++index)
            destination[cursor++] = home_prefix[index];
        for (uint32_t index = 0; status.name[index]; ++index) {
            if (cursor + 1 >= WELOVEC_USER_PATH_MAX) return 0;
            destination[cursor++] = status.name[index];
        }
        if (cursor + 1 >= WELOVEC_USER_PATH_MAX) return 0;
        destination[cursor++] = '/';
    } else {
        static const char temporary_prefix[] = "/tmp/";
        for (uint32_t index = 0; temporary_prefix[index]; ++index)
            destination[cursor++] = temporary_prefix[index];
    }
    for (uint32_t index = basename; index < end; ++index) {
        if (cursor + 1 >= WELOVEC_USER_PATH_MAX) return 0;
        destination[cursor++] = url_path[index];
    }
    destination[cursor] = '\0';
    return 1;
}

static int64_t write_downloaded_file(const char *destination,
                                     const uint8_t *data, uint32_t size,
                                     int allow_large_file) {
    if (!allow_large_file || size <= INSTALL_FS_WRITE_CHUNK)
        return welovec_fs_write(destination, data, size);

    const int64_t descriptor = welovec_open(
        destination, WELOVEC_OPEN_WRITE | WELOVEC_OPEN_CREATE |
                    WELOVEC_OPEN_TRUNCATE);
    if (descriptor < 0) return descriptor;

    uint32_t offset = 0;
    while (offset < size) {
        uint32_t chunk = size - offset;
        if (chunk > INSTALL_FS_WRITE_CHUNK) chunk = INSTALL_FS_WRITE_CHUNK;
        const int64_t written = welovec_write_fd(
            (int)descriptor, data + offset, chunk);
        if (written < 0 || (uint64_t)written != chunk) {
            const int64_t error = written < 0 ? written : -5;
            (void)welovec_close((int)descriptor);
            (void)welovec_fs_unlink(destination);
            return error;
        }
        offset += chunk;
    }

    const int64_t closed = welovec_close((int)descriptor);
    if (closed < 0) {
        (void)welovec_fs_unlink(destination);
        return closed;
    }
    return size;
}

static void register_installed_command(const char *destination) {
    static const char install_prefix[] = "/userspace/bin/shell/bin/";
    uint32_t prefix_length = 0;
    while (install_prefix[prefix_length]) ++prefix_length;
    for (uint32_t index = 0; index < prefix_length; ++index)
        if (destination[index] != install_prefix[index]) return;
    const char *filename = destination + prefix_length;
    const uint32_t filename_length = cmd_length(filename);
    if (filename_length < 5 || !cmd_has_extension(filename, ".elf")) return;
    const uint32_t name_length = filename_length - 4;
    if (!name_length || name_length >= SHELL_COMMAND_NAME_CAPACITY) return;
    for (uint32_t index = 0; index < name_length; ++index) {
        const char c = filename[index];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return;
    }

    char config_path[sizeof(SHELL_COMMAND_DIR) + SHELL_COMMAND_NAME_CAPACITY + 8];
    uint32_t cursor = 0;
    for (uint32_t index = 0; SHELL_COMMAND_DIR[index]; ++index)
        config_path[cursor++] = SHELL_COMMAND_DIR[index];
    config_path[cursor++] = '/';
    for (uint32_t index = 0; index < name_length; ++index)
        config_path[cursor++] = filename[index];
    const char suffix[] = ".conf";
    for (uint32_t index = 0; suffix[index]; ++index)
        config_path[cursor++] = suffix[index];
    config_path[cursor] = '\0';
    if (welovec_fs_exists(config_path) == 1) return;

    char config_text[512];
    cursor = 0;
    const char name_key[] = "name=";
    for (uint32_t index = 0; name_key[index]; ++index) config_text[cursor++] = name_key[index];
    for (uint32_t index = 0; index < name_length; ++index)
        config_text[cursor++] = filename[index];
    const char description[] = "\ndescription=Installed external ELF program.\nexec=";
    for (uint32_t index = 0; description[index]; ++index)
        config_text[cursor++] = description[index];
    for (uint32_t index = 0; destination[index]; ++index) {
        if (cursor + 2 >= sizeof(config_text)) return;
        config_text[cursor++] = destination[index];
    }
    config_text[cursor++] = '\n';
    const int64_t written = welovec_fs_write(config_path, config_text, cursor);
    if (written >= 0) {
        cmd_print("registered command: ", CMD_GREEN);
        for (uint32_t index = 0; index < name_length; ++index) {
            const char character[2] = {filename[index], '\0'};
            cmd_print(character, CMD_WHITE);
        }
        cmd_print(" (type help)\n", CMD_WHITE);
    }
}

void userland_main(uint64_t argc, char **argv) {
    const int all_files = argc >= 2 && cmd_equal(argv[1], "-all");
    if ((!all_files && (argc < 2 || argc > 3)) ||
        (all_files && (argc < 3 || argc > 4))) {
        cmd_print("usage: install https://host/program.elf [destination]\n", CMD_YELLOW);
        cmd_print("       install https://[2001:db8::1]/program.elf [destination]\n", CMD_YELLOW);
        cmd_print("       install -all <URL> [destination]\n", CMD_YELLOW);
        return;
    }
    const uint64_t url_argument = all_files ? 2u : 1u;
    const uint64_t destination_argument = url_argument + 1u;
    struct welovec_url parsed_url;
    if (!welovec_url_parse(argv[url_argument], &parsed_url)) {
        cmd_print("install: invalid or unsupported URL\n", CMD_RED);
        return;
    }
    const char *host = parsed_url.host;
    const char *request_path = parsed_url.path;
    const uint16_t port = parsed_url.port;
    const int secure = parsed_url.secure;
    const int ipv6_literal = parsed_url.ipv6_literal;
    char destination[WELOVEC_USER_PATH_MAX];
    if (argc > destination_argument) {
        uint32_t index = 0;
        while (argv[destination_argument][index] &&
               index + 1 < sizeof(destination)) {
            destination[index] = argv[destination_argument][index];
            ++index;
        }
        if (argv[destination_argument][index]) {
            cmd_print("install: destination path is too long\n", CMD_RED);
            return;
        }
        destination[index] = '\0';
    } else if (all_files
                   ? !make_default_file_destination(request_path, destination)
                   : !make_default_destination(request_path, destination)) {
        cmd_print("install: could not choose a destination; provide one explicitly\n",
                  CMD_RED);
        return;
    }

    if (!secure)
        cmd_print("WARNING: plain HTTP does not authenticate the server or program.\n",
                  CMD_YELLOW);
    uint8_t address4[4] = {0};
    uint8_t address6[16] = {0};
    int use_ipv6 = ipv6_literal;
    if (ipv6_literal) {
        if (!welovec_url_parse_ipv6(host, address6)) {
            cmd_print("install: invalid IPv6 address\n", CMD_RED);
            return;
        }
    } else if (!cmd_valid_ipv4(host, address4)) {
        const int64_t resolved4 = welovec_dns_lookup_ipv4(host, address4);
        if (resolved4 < 0) {
            const int64_t resolved6 = welovec_dns_lookup_ipv6(host, address6);
            if (resolved6 < 0) {
                cmd_print_error("install: DNS lookup", resolved6);
                return;
            }
            use_ipv6 = 1;
        }
    }
    const int64_t socket = use_ipv6
        ? welovec_tcp_connect_ipv6(address6, port)
        : welovec_tcp_connect_ipv4(address4, port);
    if (socket < 0) {
        cmd_print_error("install: connect", socket);
        return;
    }
    struct welovec_tls_session *tls = 0;
    if (secure) {
        int tls_error = 0;
        tls = welovec_tls_connect((int)socket, host, &tls_error);
        if (!tls) {
            (void)welovec_tcp_close((int)socket);
            cmd_print_error("install: TLS handshake or certificate validation",
                            tls_error);
            const char *tls_detail = welovec_tls_error_description(tls_error);
            if (tls_detail) {
                cmd_print("  TLS: ", CMD_YELLOW);
                cmd_print(tls_detail, CMD_YELLOW);
                cmd_print("\n", CMD_YELLOW);
            }
            return;
        }
    }
    char request[512];
    uint32_t request_length = 0;
    const char get[] = "GET ";
    for (uint32_t index = 0; get[index]; ++index) request[request_length++] = get[index];
    for (uint32_t index = 0; request_path[index]; ++index) {
        if (request_length + 4 >= sizeof(request)) break;
        request[request_length++] = request_path[index];
    }
    const char http[] = " HTTP/1.0\r\nHost: ";
    for (uint32_t index = 0; http[index]; ++index) request[request_length++] = http[index];
    if (ipv6_literal) request[request_length++] = '[';
    for (uint32_t index = 0; host[index] && request_length + 50 < sizeof(request); ++index)
        request[request_length++] = host[index];
    if (ipv6_literal) request[request_length++] = ']';
    if (port != (secure ? 443 : 80) && request_length + 8 < sizeof(request)) {
        request[request_length++] = ':';
        char port_digits[5];
        uint32_t port_count = 0;
        uint32_t port_value = port;
        do {
            port_digits[port_count++] = (char)('0' + port_value % 10);
            port_value /= 10;
        } while (port_value && port_count < sizeof(port_digits));
        while (port_count) request[request_length++] = port_digits[--port_count];
    }
    const char end[] = "\r\nConnection: close\r\n\r\n";
    for (uint32_t index = 0; end[index]; ++index) request[request_length++] = end[index];

    uint32_t sent = 0;
    while (sent < request_length) {
        uint32_t chunk = request_length - sent;
        if (chunk > WELOVEC_TCP_MAX_IO) chunk = WELOVEC_TCP_MAX_IO;
        const int64_t written = transport_send((int)socket, tls,
                                               request + sent, chunk);
        if (written <= 0) {
            close_transport((int)socket, tls);
            cmd_print_error("install: send", written);
            return;
        }
        sent += (uint32_t)written;
    }

    response_capacity = INSTALL_RESPONSE_MAX_CAPACITY;
    response_data = (uint8_t *)welovec_mmap(response_capacity);
    if (!response_data) {
        response_capacity = INSTALL_RESPONSE_FALLBACK_CAPACITY;
        response_data = (uint8_t *)welovec_mmap(response_capacity);
        if (response_data)
            cmd_print("install: 500 MiB buffer unavailable; using 250 MiB\n", CMD_YELLOW);
    }
    if (!response_data) {
        close_transport((int)socket, tls);
        cmd_print("install: insufficient memory for a 500 MiB or 250 MiB response\n",
                  CMD_RED);
        return;
    }

    uint32_t received = 0;
    uint32_t body_start = 0;
    uint32_t content_length = 0;
    int has_length = 0, chunked = 0, headers_parsed = 0;
    uint64_t start_tick = welovec_getticks();
    uint64_t last_data_tick = start_tick;
    uint64_t rate_tick = start_tick;
    uint64_t rate_allowance = INSTALL_BYTES_PER_TICK;
    int timed_out = 0;
    int response_buffer_full = 0;
    for (;;) {
        const uint64_t now = welovec_getticks();
        if (now != rate_tick) {
            rate_allowance = INSTALL_BYTES_PER_TICK;
            rate_tick = now;
        }
        if (!rate_allowance) {
            (void)welovec_sleep(1);
            continue;
        }
        uint64_t receive_capacity = response_capacity - received;
        if (receive_capacity > WELOVEC_TCP_MAX_IO)
            receive_capacity = WELOVEC_TCP_MAX_IO;
        if (receive_capacity > rate_allowance) receive_capacity = rate_allowance;
        const int64_t count = transport_receive(
            (int)socket, tls, response_data + received,
            receive_capacity);
        if (count < 0) {
            close_transport((int)socket, tls);
            cmd_print_error("install: receive", count);
            return;
        }
        if (count > 0) {
            received += (uint32_t)count;
            rate_allowance -= (uint64_t)count;
            last_data_tick = welovec_getticks();
            if (!headers_parsed && find_header_end(received, &body_start)) {
                if (body_start > INSTALL_HEADER_LIMIT ||
                    !parse_response_headers(body_start, &content_length,
                                            &has_length, &chunked) || chunked) {
                    close_transport((int)socket, tls);
                    cmd_print("install: invalid response, non-200 status, or chunked body\n",
                              CMD_RED);
                    return;
                }
                headers_parsed = 1;
            }
            if (headers_parsed && has_length &&
                received - body_start >= content_length) break;
            if (received == response_capacity) {
                response_buffer_full = 1;
                break;
            }
        } else {
            const uint64_t now = welovec_getticks();
            if (headers_parsed && now - last_data_tick >= 300) break;
            if (now - start_tick >= 3000) {
                timed_out = 1;
                break;
            }
            (void)welovec_sleep(1);
        }
    }
    close_transport((int)socket, tls);
    if (!headers_parsed || timed_out || received < body_start) {
        cmd_print("install: download timed out or returned incomplete headers\n", CMD_RED);
        return;
    }
    uint32_t body_size = received - body_start;
    if (has_length) {
        if (body_size < content_length) {
            cmd_print("install: incomplete HTTP body\n", CMD_RED);
            return;
        }
        body_size = content_length;
    } else if (response_buffer_full) {
        cmd_print("install: response exceeded the available download buffer\n",
                  CMD_RED);
        return;
    }
    if (!all_files &&
        (body_size < 6 || response_data[body_start] != 0x7f ||
         response_data[body_start + 1] != 'E' ||
         response_data[body_start + 2] != 'L' ||
         response_data[body_start + 3] != 'F' ||
         response_data[body_start + 4] != 2 ||
         response_data[body_start + 5] != 1)) {
        cmd_print("install: downloaded file is not an ELF64 little-endian executable\n",
                  CMD_RED);
        return;
    }
    if (!all_files && body_size > INSTALL_FS_WRITE_CHUNK) {
        cmd_print("install: program exceeds the filesystem write limit\n", CMD_RED);
        return;
    }
    const int64_t written = write_downloaded_file(
        destination, response_data + body_start, body_size, all_files);
    if (written < 0 || (uint64_t)written != body_size) {
        cmd_print_error("install: write", written);
        return;
    }
    if (!all_files) register_installed_command(destination);
    cmd_print(all_files ? "downloaded file " : "installed ", CMD_GREEN);
    cmd_print(destination, CMD_WHITE);
    cmd_print("\n", CMD_WHITE);
}
