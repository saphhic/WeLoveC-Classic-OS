/* Written by [@saphhic](https://github.com/saphhic)
 * Date: October 31st 2026
 * Description: Declares bounded HTML5 text rendering and link extraction.
 */

#ifndef WELOVEC_USERLAND_WEB_HTML_TEXT_H
#define WELOVEC_USERLAND_WEB_HTML_TEXT_H

#include <stddef.h>
#include <stdint.h>

#include "url.h"

#define WELOVEC_HTML_LINK_MAX 64u

struct welovec_html_link {
    char url[WELOVEC_URL_MAX_LENGTH];
};

/* Parses HTML5 with the bundled NetSurf Hubbub tokenizer and writes readable
   ASCII text. Relative HTTP(S) links are resolved against base_url and
   collected up to link_capacity. Output is NUL-terminated when capacity is
   nonzero. parser_error is set if the engine cannot parse the input. */
size_t welovec_html_to_text(const uint8_t *input, size_t input_size,
                            char *output, size_t output_capacity,
                            const char *base_url,
                            struct welovec_html_link *links,
                            size_t link_capacity, size_t *link_count,
                            int *parser_error);

#endif
