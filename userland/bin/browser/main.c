/* Written by [@saphhic](https://github.com/saphhic)
 * Date: October 31st 2026
 * Description: Provides a bounded text-only HTTP(S) browser frontend.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "user/framebuffer.h"
#include "user/input.h"
#include "user/network.h"
#include "user/syscall.h"
#include "lib/web/http.h"
#include "lib/web/html_text.h"
#include "lib/tls/tls_client.h"

#define BROWSER_BODY_CAPACITY (256u * 1024u)
#define BROWSER_ADDRESS_CAPACITY WELOVEC_URL_MAX_LENGTH
#define BROWSER_INPUT_CAPACITY WELOVEC_URL_MAX_LENGTH
#define BROWSER_COLUMNS_MAX 160u
#define BROWSER_HISTORY_MAX 16u

static uint8_t *g_response_body;
static char *g_page_text;
static size_t g_page_size;
static char g_url[BROWSER_ADDRESS_CAPACITY];
static char g_history[BROWSER_HISTORY_MAX][BROWSER_ADDRESS_CAPACITY];
static size_t g_history_count;
static size_t g_history_index;
static struct welovec_html_link g_links[WELOVEC_HTML_LINK_MAX];
static size_t g_link_count;
static size_t g_selected_link;
static char g_edit[BROWSER_INPUT_CAPACITY];
static char g_status[192];
static uint32_t g_scroll_row;
static uint32_t g_total_rows;
static uint32_t g_columns;
static int g_editing;
static size_t g_edit_cursor;

static size_t text_length(const char *text);
static void set_status(const char *message);

static void copy_text(char *destination, size_t capacity, const char *source) {
    size_t index = 0;
    if (!destination || !capacity) return;
    if (source) {
        while (source[index] && index + 1 < capacity) {
            destination[index] = source[index];
            ++index;
        }
    }
    destination[index] = '\0';
}

static size_t append_number(char *destination, size_t capacity, size_t used,
                            size_t number) {
    char digits[24];
    size_t count = 0;
    if (!destination || used >= capacity) return used;
    do {
        digits[count++] = (char)('0' + number % 10);
        number /= 10;
    } while (number && count < sizeof(digits));
    while (count && used + 1 < capacity)
        destination[used++] = digits[--count];
    destination[used] = '\0';
    return used;
}

static void set_link_status(void) {
    char message[192];
    size_t used = 0;
    copy_text(message, sizeof(message), "Enlace ");
    used = text_length(message);
    used = append_number(message, sizeof(message), used, g_selected_link + 1);
    if (used + 1 < sizeof(message)) message[used++] = '/';
    used = append_number(message, sizeof(message), used, g_link_count);
    static const char open_text[] = " Enter abre: ";
    for (size_t index = 0; open_text[index] && used + 1 < sizeof(message); ++index)
        message[used++] = open_text[index];
    message[used] = '\0';
    if (g_link_count && g_selected_link < g_link_count)
        copy_text(message + used, sizeof(message) - used,
                  g_links[g_selected_link].url);
    set_status(message);
}

static void record_history(const char *url) {
    if (!url || !url[0]) return;
    if (g_history_count &&
        strcmp(g_history[g_history_index], url) == 0) return;
    g_history_count = g_history_index + 1;
    if (g_history_count == BROWSER_HISTORY_MAX) {
        for (size_t index = 1; index < g_history_count; ++index)
            copy_text(g_history[index - 1], sizeof(g_history[index - 1]),
                      g_history[index]);
        --g_history_count;
        if (g_history_index) --g_history_index;
    }
    copy_text(g_history[g_history_count], sizeof(g_history[g_history_count]), url);
    g_history_index = g_history_count++;
}

static size_t text_length(const char *text) {
    size_t length = 0;
    if (!text) return 0;
    while (text[length]) ++length;
    return length;
}

static int starts_with_html_type(const char *content_type) {
    static const char html[] = "text/html";
    for (size_t index = 0; index < sizeof(html) - 1; ++index) {
        char value = content_type[index];
        if (value >= 'A' && value <= 'Z') value = (char)(value - 'A' + 'a');
        if (value != html[index]) return 0;
    }
    return 1;
}

static int starts_with_ci(const char *text, const char *prefix) {
    for (size_t index = 0; prefix[index]; ++index) {
        char value = text[index];
        if (value >= 'A' && value <= 'Z') value = (char)(value - 'A' + 'a');
        char wanted = prefix[index];
        if (wanted >= 'A' && wanted <= 'Z')
            wanted = (char)(wanted - 'A' + 'a');
        if (value != wanted) return 0;
    }
    return 1;
}

static size_t copy_plain_text(const uint8_t *input, size_t input_size,
                              char *output, size_t output_capacity) {
    size_t written = 0;
    if (!output || !output_capacity) return 0;
    for (size_t index = 0; index < input_size && written + 1 < output_capacity;
         ++index) {
        const uint8_t character = input[index];
        if (character == '\r') continue;
        if (character == '\n' || character == '\t' ||
            (character >= 0x20 && character <= 0x7e))
            output[written++] = (char)character;
        else
            output[written++] = '?';
    }
    output[written] = '\0';
    return written;
}

static uint32_t visible_rows(uint32_t height) {
    if (height <= 84u) return 1;
    return (height - 84u) / 16u;
}

static uint32_t count_page_rows(uint32_t columns) {
    uint32_t rows = 1;
    uint32_t column = 0;
    for (size_t index = 0; index < g_page_size; ++index) {
        if (g_page_text[index] == '\n') {
            ++rows;
            column = 0;
        } else {
            if (column >= columns) {
                ++rows;
                column = 0;
            }
            ++column;
        }
    }
    return rows;
}

static void draw_wrapped_page(uint32_t width, uint32_t height) {
    const uint32_t columns = g_columns ? g_columns : 1;
    const uint32_t rows = visible_rows(height);
    const uint32_t first_row = g_scroll_row;
    const uint32_t last_row = first_row + rows;
    uint32_t row = 0;
    uint32_t column = 0;
    char line[BROWSER_COLUMNS_MAX + 1u];
    size_t line_length = 0;
    const int32_t left = 8;
    const int32_t first_y = 64;

    for (size_t index = 0; index < g_page_size; ++index) {
        const char character = g_page_text[index];
        if (character == '\n' || column >= columns) {
            line[line_length] = '\0';
            if (row >= first_row && row < last_row)
                (void)welovec_fb_text(left, first_y +
                    (int32_t)(row - first_row) * 16, line,
                    welovec_fb_rgb(226, 231, 240));
            ++row;
            column = 0;
            line_length = 0;
            if (character == '\n') continue;
        }
        if (line_length < BROWSER_COLUMNS_MAX)
            line[line_length++] = character;
        ++column;
    }
    line[line_length] = '\0';
    if (row >= first_row && row < last_row)
        (void)welovec_fb_text(left, first_y +
            (int32_t)(row - first_row) * 16, line,
            welovec_fb_rgb(226, 231, 240));
    (void)width;
}

static void draw_screen(void) {
    struct welovec_fb_info info;
    char line[BROWSER_COLUMNS_MAX + 1u];
    if (welovec_fb_get_info(&info) < 0 || !info.width || !info.height) return;
    const uint32_t width = info.width;
    const uint32_t height = info.height;
    g_columns = width > 24u ? (width - 16u) / 8u : 1u;
    if (g_columns > BROWSER_COLUMNS_MAX) g_columns = BROWSER_COLUMNS_MAX;
    g_total_rows = count_page_rows(g_columns);
    const uint32_t screen_rows = visible_rows(height);
    if (g_scroll_row + screen_rows > g_total_rows)
        g_scroll_row = g_total_rows > screen_rows ? g_total_rows - screen_rows : 0;

    (void)welovec_fb_rect(0, 0, (int32_t)width, (int32_t)height,
                          welovec_fb_rgb(18, 24, 35));
    (void)welovec_fb_rect(0, 0, (int32_t)width, 58,
                          welovec_fb_rgb(35, 46, 63));
    (void)welovec_fb_text(8, 4, "WeLoveC Text Browser",
                          welovec_fb_rgb(255, 255, 255));
    const char *address = g_editing ? g_edit : g_url;
    const char *prefix = g_editing ? "URL> " : "URL: ";
    size_t used = 0;
    while (prefix[used] && used < sizeof(line) - 1) {
        line[used] = prefix[used];
        ++used;
    }
    const size_t address_length = text_length(address);
    for (size_t address_index = 0; address_index <= address_length &&
         used + 1 < sizeof(line); ++address_index) {
        if (g_editing && address_index == g_edit_cursor &&
            used + 1 < sizeof(line))
            line[used++] = '|';
        if (address_index < address_length && used + 1 < sizeof(line))
            line[used++] = address[address_index];
    }
    if (used > g_columns && g_columns > 3) {
        line[g_columns - 3] = '.';
        line[g_columns - 2] = '.';
        line[g_columns - 1] = '.';
        used = g_columns;
    }
    line[used] = '\0';
    (void)welovec_fb_text(8, 24, line, welovec_fb_rgb(164, 213, 255));
    (void)welovec_fb_text(8, 42, g_status,
                          welovec_fb_rgb(204, 210, 221));

    if (g_page_size) draw_wrapped_page(width, height);
    if (height >= 18) {
        (void)welovec_fb_rect(0, (int32_t)height - 18, (int32_t)width, 18,
                              welovec_fb_rgb(35, 46, 63));
        (void)welovec_fb_text(8, (int32_t)height - 17,
                              "u URL  r recargar  [] enlaces/Enter abrir  h/f historial  j/k  q salir",
                              welovec_fb_rgb(190, 197, 210));
    }
}

static void set_status(const char *message) {
    copy_text(g_status, sizeof(g_status), message);
}

static int load_address(const char *address_text, int add_to_history) {
    char requested[WELOVEC_URL_MAX_LENGTH];
    struct welovec_http_response response;
    size_t address_length = text_length(address_text);
    if (!address_length) {
        set_status("Escribe una dirección HTTPS o HTTP con u.");
        draw_screen();
        return 0;
    }
    if (address_length >= sizeof(requested)) {
        set_status("La dirección supera el límite de longitud.");
        draw_screen();
        return 0;
    }
    if ((address_length >= 7 && starts_with_ci(address_text, "http://")) ||
        (address_length >= 8 && starts_with_ci(address_text, "https://"))) {
        copy_text(requested, sizeof(requested), address_text);
    } else {
        static const char secure_prefix[] = "https://";
        memcpy(requested, secure_prefix, sizeof(secure_prefix) - 1);
        memcpy(requested + sizeof(secure_prefix) - 1, address_text,
               address_length + 1);
    }

    set_status("Cargando página...");
    draw_screen();
    int tls_error = 0;
    const int result = welovec_http_get(requested, g_response_body,
        BROWSER_BODY_CAPACITY, &response, &tls_error);
    if (result != WELOVEC_HTTP_OK) {
        if (result == WELOVEC_HTTP_TLS_ERROR) {
            const char *description = welovec_tls_error_description(tls_error);
            if (description) copy_text(g_status, sizeof(g_status), description);
            else set_status("TLS no pudo validar el certificado. No se cambió a HTTP.");
        } else {
            copy_text(g_status, sizeof(g_status),
                      welovec_http_error_string(result));
        }
        draw_screen();
        return 0;
    }

    copy_text(g_url, sizeof(g_url), response.final_url);
    g_link_count = 0;
    g_selected_link = 0;
    int html_parse_error = 0;
    if (starts_with_html_type(response.content_type)) {
        g_page_size = welovec_html_to_text(g_response_body, response.body_size,
                                           g_page_text,
                                           BROWSER_BODY_CAPACITY + 1u,
                                           response.final_url, g_links,
                                           WELOVEC_HTML_LINK_MAX,
                                           &g_link_count, &html_parse_error);
        if (html_parse_error) {
            static const char parser_message[] =
                "No se pudo analizar el documento HTML.";
            copy_text(g_page_text, BROWSER_BODY_CAPACITY + 1u,
                      parser_message);
            g_page_size = text_length(g_page_text);
            g_link_count = 0;
        }
    } else {
        g_page_size = copy_plain_text(g_response_body, response.body_size,
                                      g_page_text,
                                      BROWSER_BODY_CAPACITY + 1u);
    }
    g_scroll_row = 0;
    if (add_to_history) record_history(response.final_url);
    char status_line[192];
    const uint16_t code = response.status;
    status_line[0] = 'H'; status_line[1] = 'T'; status_line[2] = 'T';
    status_line[3] = 'P'; status_line[4] = ' ';
    status_line[5] = (char)('0' + code / 100u);
    status_line[6] = (char)('0' + (code / 10u) % 10u);
    status_line[7] = (char)('0' + code % 10u);
    status_line[8] = ' ';
    const char *type = response.content_type[0]
        ? response.content_type : "contenido";
    copy_text(status_line + 9, sizeof(status_line) - 9, type);
    if (starts_with_ci(response.final_url, "http://")) {
        const size_t status_length = text_length(status_line);
        static const char insecure_note[] = " (sin cifrar)";
        if (status_length + sizeof(insecure_note) <= sizeof(status_line))
            memcpy(status_line + status_length, insecure_note,
                   sizeof(insecure_note));
    }
    set_status(status_line);
    draw_screen();
    return 1;
}

static void navigate_history(int direction) {
    if (direction < 0) {
        if (!g_history_count || !g_history_index) {
            set_status("No hay una página anterior en el historial.");
            draw_screen();
            return;
        }
        const size_t target = g_history_index - 1;
        if (load_address(g_history[target], 0)) g_history_index = target;
    } else {
        if (!g_history_count || g_history_index + 1 >= g_history_count) {
            set_status("No hay una página siguiente en el historial.");
            draw_screen();
            return;
        }
        const size_t target = g_history_index + 1;
        if (load_address(g_history[target], 0)) g_history_index = target;
    }
}

static void open_selected_link(void) {
    if (!g_link_count || g_selected_link >= g_link_count) {
        set_status("Esta página no tiene enlaces HTTP o HTTPS utilizables.");
        draw_screen();
        return;
    }
    (void)load_address(g_links[g_selected_link].url, 1);
}

static void select_link(int direction) {
    if (!g_link_count) {
        set_status("Esta página no tiene enlaces HTTP o HTTPS utilizables.");
        draw_screen();
        return;
    }
    if (direction < 0)
        g_selected_link = g_selected_link ? g_selected_link - 1 : g_link_count - 1;
    else
        g_selected_link = (g_selected_link + 1) % g_link_count;
    set_link_status();
    draw_screen();
}

static void begin_address_edit(void) {
    copy_text(g_edit, sizeof(g_edit), g_url);
    g_edit_cursor = text_length(g_edit);
    g_editing = 1;
    set_status("Escribe la dirección y pulsa Enter; Escape cancela.");
    draw_screen();
}

static void cancel_address_edit(void) {
    g_editing = 0;
    set_status("Navegación cancelada.");
    draw_screen();
}

static void accept_address_edit(void) {
    g_editing = 0;
    (void)load_address(g_edit, 1);
}

static void scroll_by(int32_t amount) {
    struct welovec_fb_info info;
    uint32_t page_rows;
    if (welovec_fb_get_info(&info) < 0) return;
    page_rows = visible_rows(info.height);
    if (amount < 0) {
        const uint32_t decrease = (uint32_t)(-amount);
        g_scroll_row = g_scroll_row > decrease ? g_scroll_row - decrease : 0;
    } else {
        uint32_t maximum = g_total_rows > page_rows
            ? g_total_rows - page_rows : 0;
        if (g_scroll_row + (uint32_t)amount > maximum)
            g_scroll_row = maximum;
        else
            g_scroll_row += (uint32_t)amount;
    }
    draw_screen();
}

static void handle_navigation_key(char key) {
    struct welovec_fb_info info;
    if (key == 'q') {
        set_status("Navegador finalizado; cierra esta ventana con X.");
        draw_screen();
        welovec_exit(0);
    } else if (key == 'u') {
        begin_address_edit();
    } else if (key == 'r') {
        if (g_url[0]) (void)load_address(g_url, 0);
        else begin_address_edit();
    } else if (key == 'h') {
        navigate_history(-1);
    } else if (key == 'f') {
        navigate_history(1);
    } else if (key == '[') {
        select_link(-1);
    } else if (key == ']') {
        select_link(1);
    } else if (key == '\n' || key == '\r') {
        open_selected_link();
    } else if (key == 'j' || key == ' ') {
        scroll_by(key == ' ' && welovec_fb_get_info(&info) == 0
            ? (int32_t)(visible_rows(info.height) > 1
                ? visible_rows(info.height) - 1 : 1) : 1);
    } else if (key == 'k') {
        scroll_by(-1);
    } else if (key == 'b' && welovec_fb_get_info(&info) == 0) {
        scroll_by(-(int32_t)(visible_rows(info.height) > 1
            ? visible_rows(info.height) - 1 : 1));
    }
}

static void handle_edit_key(char key) {
    size_t length = text_length(g_edit);
    if (key == 0x1b) {
        cancel_address_edit();
    } else if (key == '\n' || key == '\r') {
        accept_address_edit();
    } else if (key == '\b' || key == 0x7f) {
        if (g_edit_cursor) {
            memmove(g_edit + g_edit_cursor - 1, g_edit + g_edit_cursor,
                    length - g_edit_cursor + 1);
            --g_edit_cursor;
        }
        draw_screen();
    } else if ((uint8_t)key >= 0x20 && (uint8_t)key <= 0x7e &&
               length + 1 < sizeof(g_edit)) {
        memmove(g_edit + g_edit_cursor + 1, g_edit + g_edit_cursor,
                length - g_edit_cursor + 1);
        g_edit[g_edit_cursor++] = key;
        draw_screen();
    }
}

static void handle_arrow(char key) {
    if (g_editing) {
        const size_t length = text_length(g_edit);
        if (key == 'D' && g_edit_cursor) --g_edit_cursor;
        else if (key == 'C' && g_edit_cursor < length) ++g_edit_cursor;
        draw_screen();
    } else if (key == 'A') {
        scroll_by(-1);
    } else if (key == 'B') {
        scroll_by(1);
    }
}

static void handle_input(const char *input, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        if ((uint8_t)input[index] != 0x1b) {
            if (g_editing) handle_edit_key(input[index]);
            else handle_navigation_key(input[index]);
            continue;
        }
        if (index + 2 < count && input[index + 1] == '[') {
            const char key = input[index + 2];
            if (key == 'A' || key == 'B' || key == 'C' || key == 'D') {
                handle_arrow(key);
                index += 2;
                continue;
            }
            if ((key == '5' || key == '6') && index + 3 < count &&
                input[index + 3] == '~') {
                struct welovec_fb_info info;
                if (welovec_fb_get_info(&info) == 0) {
                    const int32_t page = (int32_t)(visible_rows(info.height) > 1
                        ? visible_rows(info.height) - 1 : 1);
                    scroll_by(key == '5' ? -page : page);
                }
                index += 3;
                continue;
            }
        }
        if (g_editing) cancel_address_edit();
    }
}

void userland_main(uint64_t argc, char **argv) {
    char input[32];
    g_response_body = (uint8_t *)malloc(BROWSER_BODY_CAPACITY);
    g_page_text = (char *)malloc(BROWSER_BODY_CAPACITY + 1u);
    if (!g_response_body || !g_page_text) {
        set_status("No hay memoria para abrir el navegador.");
        draw_screen();
        welovec_exit(1);
    }
    g_page_text[0] = '\0';
    g_page_size = 0;
    g_scroll_row = 0;
    g_total_rows = 1;
    g_url[0] = '\0';
    g_history_count = 0;
    g_history_index = 0;
    g_link_count = 0;
    g_selected_link = 0;
    g_status[0] = '\0';
    if (argc > 1 && argv && argv[1]) {
        (void)load_address(argv[1], 1);
    } else {
        begin_address_edit();
    }

    for (;;) {
        const int64_t count = welovec_input_read(input, sizeof(input));
        if (count < 0) {
            set_status("No se pudo leer el teclado.");
            draw_screen();
            (void)welovec_sleep(1);
            continue;
        }
        handle_input(input, (size_t)count);
    }
}
