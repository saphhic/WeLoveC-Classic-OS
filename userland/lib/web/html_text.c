/* Written by [@saphhic](https://github.com/saphhic)
 * Date: October 31st 2026
 * Description: Renders HTML5 tokens from the bundled NetSurf Hubbub parser.
 */

#include "html_text.h"

#include <hubbub/hubbub.h>
#include <hubbub/parser.h>

#include "url.h"

struct text_writer {
    char *output;
    size_t capacity;
    size_t length;
    uint8_t pending_space;
    uint8_t preserve_space;
};

struct html_context {
    struct text_writer writer;
    hubbub_parser *parser;
    char base_url[WELOVEC_URL_MAX_LENGTH];
    struct welovec_html_link *links;
    size_t link_capacity;
    size_t link_count;
    size_t active_link;
    uint8_t skip_head;
    uint8_t skip_script;
    uint8_t skip_style;
    uint8_t base_seen;
};

static uint8_t ascii_lower(uint8_t value) {
    return value >= 'A' && value <= 'Z'
        ? (uint8_t)(value - 'A' + 'a') : value;
}

static int span_equal_ci(hubbub_string span, const char *literal) {
    size_t index = 0;
    while (literal[index]) {
        if (index >= span.len || ascii_lower(span.ptr[index]) !=
            (uint8_t)literal[index]) return 0;
        ++index;
    }
    return index == span.len;
}

static void write_byte(struct text_writer *writer, char value) {
    if (!writer || writer->length + 1 >= writer->capacity) return;
    writer->output[writer->length++] = value;
    writer->output[writer->length] = '\0';
}

static void write_space(struct text_writer *writer) {
    if (writer->length) writer->pending_space = 1;
}

static void write_ascii_text(struct text_writer *writer, uint8_t value) {
    if (value == '\r') return;
    if (value == '\n' || value == '\t' || value == ' ') {
        if (writer->preserve_space) {
            write_byte(writer, value == '\t' ? ' ' : (char)value);
        } else {
            write_space(writer);
        }
        return;
    }
    if (value < 0x20) return;
    if (writer->pending_space && writer->length &&
        writer->output[writer->length - 1] != '\n')
        write_byte(writer, ' ');
    writer->pending_space = 0;
    write_byte(writer, value < 0x7f ? (char)value : '?');
}

static void write_utf8_text(struct text_writer *writer,
                            const uint8_t *text, size_t length) {
    for (size_t index = 0; index < length; ++index) {
        const uint8_t value = text[index];
        if (value >= 0x80 && value <= 0xbf) continue;
        write_ascii_text(writer, value);
    }
}

static void write_newline(struct text_writer *writer) {
    writer->pending_space = 0;
    while (writer->length && writer->output[writer->length - 1] == ' ')
        writer->output[--writer->length] = '\0';
    if (writer->length && writer->output[writer->length - 1] != '\n')
        write_byte(writer, '\n');
}

static void write_link_marker(struct text_writer *writer, size_t number) {
    char digits[24];
    size_t length = 0;
    if (writer->length && writer->output[writer->length - 1] != '\n' &&
        writer->output[writer->length - 1] != ' ')
        write_byte(writer, ' ');
    writer->pending_space = 0;
    while (number && length < sizeof(digits)) {
        digits[length++] = (char)('0' + number % 10);
        number /= 10;
    }
    write_byte(writer, '[');
    while (length) write_byte(writer, digits[--length]);
    write_byte(writer, ']');
    write_byte(writer, ' ');
    writer->pending_space = 0;
}

static int is_block_tag(hubbub_string name) {
    return span_equal_ci(name, "address") || span_equal_ci(name, "article") ||
        span_equal_ci(name, "aside") || span_equal_ci(name, "blockquote") ||
        span_equal_ci(name, "dd") || span_equal_ci(name, "div") ||
        span_equal_ci(name, "dl") || span_equal_ci(name, "dt") ||
        span_equal_ci(name, "fieldset") || span_equal_ci(name, "figcaption") ||
        span_equal_ci(name, "figure") || span_equal_ci(name, "footer") ||
        span_equal_ci(name, "form") || span_equal_ci(name, "h1") ||
        span_equal_ci(name, "h2") || span_equal_ci(name, "h3") ||
        span_equal_ci(name, "h4") || span_equal_ci(name, "h5") ||
        span_equal_ci(name, "h6") || span_equal_ci(name, "header") ||
        span_equal_ci(name, "li") || span_equal_ci(name, "main") ||
        span_equal_ci(name, "nav") || span_equal_ci(name, "ol") ||
        span_equal_ci(name, "p") || span_equal_ci(name, "section") ||
        span_equal_ci(name, "table") || span_equal_ci(name, "tr") ||
        span_equal_ci(name, "ul");
}

static const hubbub_string *find_attribute(const hubbub_tag *tag,
                                           const char *name) {
    for (uint32_t index = 0; index < tag->n_attributes; ++index) {
        if (span_equal_ci(tag->attributes[index].name, name))
            return &tag->attributes[index].value;
    }
    return NULL;
}

static int resolve_href(struct html_context *context, hubbub_string href,
                        char output[WELOVEC_URL_MAX_LENGTH]) {
    char reference[WELOVEC_URL_MAX_LENGTH];
    struct welovec_url base;
    struct welovec_url resolved;
    const size_t length = href.len;
    if (!length || length >= sizeof(reference)) return 0;
    for (size_t index = 0; index < length; ++index) {
        if (href.ptr[index] < 0x20 || href.ptr[index] >= 0x7f) return 0;
        reference[index] = (char)href.ptr[index];
    }
    reference[length] = '\0';
    return welovec_url_parse(context->base_url, &base) &&
        welovec_url_resolve(&base, reference, &resolved) &&
        welovec_url_format(&resolved, output);
}

static size_t add_link(struct html_context *context, hubbub_string href) {
    if (!context->links || context->link_count >= context->link_capacity ||
        !context->base_url[0])
        return (size_t)-1;
    if (!resolve_href(context, href,
        context->links[context->link_count].url)) return (size_t)-1;
    return context->link_count++;
}

static void set_content_model(hubbub_parser *parser,
                              hubbub_content_model model) {
    hubbub_parser_optparams params;
    params.content_model.model = model;
    (void)hubbub_parser_setopt(parser, HUBBUB_PARSER_CONTENT_MODEL, &params);
}

static hubbub_error html_token_handler(const hubbub_token *token, void *pw) {
    struct html_context *context = (struct html_context *)pw;
    struct text_writer *writer = &context->writer;
    if (token->type == HUBBUB_TOKEN_CHARACTER) {
        if (!context->skip_head && !context->skip_script &&
            !context->skip_style)
            write_utf8_text(writer, token->data.character.ptr,
                            token->data.character.len);
        return HUBBUB_OK;
    }
    if (token->type != HUBBUB_TOKEN_START_TAG &&
        token->type != HUBBUB_TOKEN_END_TAG) return HUBBUB_OK;

    const int closing = token->type == HUBBUB_TOKEN_END_TAG;
    const hubbub_string name = token->data.tag.name;
    if (span_equal_ci(name, "head")) {
        context->skip_head = (uint8_t)!closing;
        return HUBBUB_OK;
    }
    if (span_equal_ci(name, "script")) {
        context->skip_script = (uint8_t)!closing;
        if (closing) write_newline(writer);
        else set_content_model(context->parser, HUBBUB_CONTENT_MODEL_CDATA);
        return HUBBUB_OK;
    }
    if (span_equal_ci(name, "style")) {
        context->skip_style = (uint8_t)!closing;
        if (closing) write_newline(writer);
        else set_content_model(context->parser, HUBBUB_CONTENT_MODEL_CDATA);
        return HUBBUB_OK;
    }
    if (span_equal_ci(name, "title") || span_equal_ci(name, "textarea")) {
        set_content_model(context->parser, HUBBUB_CONTENT_MODEL_RCDATA);
        return HUBBUB_OK;
    }
    if (span_equal_ci(name, "base")) {
        if (!closing && context->skip_head && !context->base_seen) {
            const hubbub_string *href = find_attribute(&token->data.tag,
                                                        "href");
            if (href && resolve_href(context, *href, context->base_url))
                context->base_seen = 1;
        }
        return HUBBUB_OK;
    }
    if (context->skip_head || context->skip_script || context->skip_style)
        return HUBBUB_OK;

    if (span_equal_ci(name, "a")) {
        if (closing) {
            context->active_link = (size_t)-1;
        } else {
            const hubbub_string *href = find_attribute(&token->data.tag,
                                                        "href");
            if (href) context->active_link = add_link(context, *href);
            if (context->active_link != (size_t)-1)
                write_link_marker(writer, context->active_link + 1);
        }
        return HUBBUB_OK;
    }
    if (closing && span_equal_ci(name, "pre")) writer->preserve_space = 0;
    if (is_block_tag(name)) write_newline(writer);
    if (!closing && span_equal_ci(name, "li")) {
        write_byte(writer, '*');
        write_byte(writer, ' ');
    }
    if (!closing && span_equal_ci(name, "img")) {
        const hubbub_string *alt = find_attribute(&token->data.tag, "alt");
        if (alt && alt->len) {
            write_byte(writer, '[');
            write_utf8_text(writer, alt->ptr, alt->len);
            write_byte(writer, ']');
        }
    }
    if (span_equal_ci(name, "br") || span_equal_ci(name, "hr"))
        write_newline(writer);
    if (!closing && span_equal_ci(name, "pre")) writer->preserve_space = 1;
    return HUBBUB_OK;
}

size_t welovec_html_to_text(const uint8_t *input, size_t input_size,
                            char *output, size_t output_capacity,
                            const char *base_url,
                            struct welovec_html_link *links,
                            size_t link_capacity, size_t *link_count,
                            int *parser_error) {
    struct html_context context;
    hubbub_parser_optparams params;
    if (link_count) *link_count = 0;
    if (parser_error) *parser_error = 0;
    if (!output || !output_capacity || !input) return 0;
    output[0] = '\0';
    context.writer.output = output;
    context.writer.capacity = output_capacity;
    context.writer.length = 0;
    context.writer.pending_space = 0;
    context.writer.preserve_space = 0;
    context.base_url[0] = '\0';
    if (base_url) {
        size_t index = 0;
        while (base_url[index] && index + 1 < sizeof(context.base_url)) {
            context.base_url[index] = base_url[index];
            ++index;
        }
        context.base_url[index] = '\0';
    }
    context.links = links;
    context.link_capacity = link_capacity;
    context.link_count = 0;
    context.active_link = (size_t)-1;
    context.skip_head = 0;
    context.skip_script = 0;
    context.skip_style = 0;
    context.base_seen = 0;

    if (hubbub_parser_create(NULL, false, &context.parser) != HUBBUB_OK) {
        if (parser_error) *parser_error = 1;
        return 0;
    }
    params.token_handler.handler = html_token_handler;
    params.token_handler.pw = &context;
    if (hubbub_parser_setopt(context.parser, HUBBUB_PARSER_TOKEN_HANDLER,
                             &params) != HUBBUB_OK ||
        hubbub_parser_parse_chunk(context.parser, input, input_size) != HUBBUB_OK ||
        hubbub_parser_completed(context.parser) != HUBBUB_OK) {
        if (parser_error) *parser_error = 1;
    }
    (void)hubbub_parser_destroy(context.parser);
    while (context.writer.length &&
           (output[context.writer.length - 1] == ' ' ||
            output[context.writer.length - 1] == '\n')) {
        output[--context.writer.length] = '\0';
    }
    if (link_count) *link_count = context.link_count;
    return context.writer.length;
}
