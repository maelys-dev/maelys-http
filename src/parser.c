#include "internal.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef enum parser_state {
    STATE_START = 0,
    STATE_HEADERS,
    STATE_FIXED,
    STATE_UNTIL_EOF,
    STATE_CHUNK_SIZE,
    STATE_CHUNK_DATA,
    STATE_CHUNK_CR,
    STATE_CHUNK_LF,
    STATE_TRAILERS,
    STATE_COMPLETE,
    STATE_ERROR
} parser_state_t;

struct maelys_http_parser {
    maelys_http_parser_kind_t kind;
    maelys_http_limits_t limits;
    maelys_http_body_fn body_fn;
    void *body_context;
    parser_state_t state;
    maelys_http_result_t result;
    int headers_complete;
    int response_to_head;
    int response_to_connect;
    char *line;
    size_t line_length;
    size_t line_capacity;
    char *method;
    char *target;
    char *version;
    char *reason;
    unsigned status;
    owned_header_t *headers;
    size_t header_count;
    size_t header_bytes;
    owned_header_t *trailers;
    size_t trailer_count;
    size_t trailer_bytes;
    maelys_http_body_framing_t framing;
    uint64_t content_length;
    uint64_t body_bytes;
    uint64_t remaining;
    uint64_t chunk_remaining;
};

static maelys_http_slice_t empty_slice(void) {
    maelys_http_slice_t slice = {NULL, 0u};
    return slice;
}

static maelys_http_header_view_t empty_header(void) {
    maelys_http_header_view_t header = {empty_slice(), empty_slice()};
    return header;
}

static maelys_http_result_t fail(
    maelys_http_parser_t *parser, maelys_http_result_t result) {
    parser->state = STATE_ERROR;
    parser->result = result;
    return result;
}

static int limits_valid(const maelys_http_limits_t *limits) {
    return limits && limits->max_start_line_bytes >= 16u &&
        limits->max_start_line_bytes < SIZE_MAX &&
        limits->max_header_line_bytes >= 2u && limits->max_header_bytes >= 2u &&
        limits->max_header_line_bytes < SIZE_MAX &&
        limits->max_header_count > 0u && limits->max_body_bytes > 0u &&
        limits->max_chunk_line_bytes >= 3u &&
        limits->max_chunk_line_bytes < SIZE_MAX && limits->max_trailer_bytes >= 2u &&
        limits->max_trailer_count > 0u;
}

static void clear_message(maelys_http_parser_t *parser) {
    free(parser->method);
    free(parser->target);
    free(parser->version);
    free(parser->reason);
    parser->method = NULL;
    parser->target = NULL;
    parser->version = NULL;
    parser->reason = NULL;
    parser->status = 0u;
    maelys_http_internal_free_headers(parser->headers, parser->header_count);
    parser->headers = NULL;
    parser->header_count = 0u;
    parser->header_bytes = 0u;
    maelys_http_internal_free_headers(parser->trailers, parser->trailer_count);
    parser->trailers = NULL;
    parser->trailer_count = 0u;
    parser->trailer_bytes = 0u;
    parser->framing = MAELYS_HTTP_BODY_NONE;
    parser->content_length = 0u;
    parser->body_bytes = 0u;
    parser->remaining = 0u;
    parser->chunk_remaining = 0u;
    parser->headers_complete = 0;
    parser->line_length = 0u;
}

maelys_http_result_t maelys_http_parser_create(
    maelys_http_parser_kind_t kind, const maelys_http_limits_t *limits,
    maelys_http_body_fn body_fn, void *body_context,
    maelys_http_parser_t **out_parser) {
    maelys_http_limits_t defaults;
    maelys_http_parser_t *parser;
    if (out_parser) *out_parser = NULL;
    if (!out_parser || (kind != MAELYS_HTTP_PARSE_REQUEST &&
                        kind != MAELYS_HTTP_PARSE_RESPONSE)) {
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    if (!limits) {
        maelys_http_limits_default(&defaults);
        limits = &defaults;
    }
    if (!limits_valid(limits)) return MAELYS_HTTP_ERR_ARGUMENT;
    parser = calloc(1u, sizeof(*parser));
    if (!parser) return MAELYS_HTTP_ERR_MEMORY;
    parser->kind = kind;
    parser->limits = *limits;
    parser->body_fn = body_fn;
    parser->body_context = body_context;
    parser->state = STATE_START;
    parser->result = MAELYS_HTTP_AGAIN;
    *out_parser = parser;
    return MAELYS_HTTP_OK;
}

maelys_http_result_t maelys_http_parser_set_response_to_head(
    maelys_http_parser_t *parser, int response_to_head) {
    if (!parser || parser->kind != MAELYS_HTTP_PARSE_RESPONSE ||
        (response_to_head != 0 && response_to_head != 1) ||
        parser->state != STATE_START || parser->line_length != 0u) {
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    parser->response_to_head = response_to_head;
    return MAELYS_HTTP_OK;
}

maelys_http_result_t maelys_http_parser_set_response_to_connect(
    maelys_http_parser_t *parser, int response_to_connect) {
    if (!parser || parser->kind != MAELYS_HTTP_PARSE_RESPONSE ||
        (response_to_connect != 0 && response_to_connect != 1) ||
        parser->state != STATE_START || parser->line_length != 0u) {
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    parser->response_to_connect = response_to_connect;
    return MAELYS_HTTP_OK;
}

static int valid_method(const char *method, size_t length) {
    size_t index;
    if (!length) return 0;
    for (index = 0; index < length; ++index) {
        if (!maelys_http_internal_is_tchar((unsigned char)method[index])) return 0;
    }
    return 1;
}

static maelys_http_result_t parse_start_line(maelys_http_parser_t *parser) {
    char *first;
    char *second;
    char *line = parser->line;
    size_t length = parser->line_length;
    if (!length || memchr(line, '\0', length)) return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
    line[length] = '\0';
    if (parser->kind == MAELYS_HTTP_PARSE_REQUEST) {
        first = strchr(line, ' ');
        if (!first) return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
        second = strchr(first + 1, ' ');
        if (!second || strchr(second + 1, ' ')) return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
        if (!valid_method(line, (size_t)(first - line)) ||
            !maelys_http_internal_method_target_valid(
                line, (size_t)(first - line), first + 1,
                (size_t)(second - first - 1)) ||
            strcmp(second + 1, "HTTP/1.1") != 0) {
            return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
        }
        parser->method = maelys_http_internal_strndup(line, (size_t)(first - line));
        parser->target = maelys_http_internal_strndup(first + 1, (size_t)(second - first - 1));
        parser->version = maelys_http_internal_strdup("HTTP/1.1");
    } else {
        if (length < 13u || memcmp(line, "HTTP/1.1 ", 9u) != 0 ||
            !isdigit((unsigned char)line[9]) ||
            !isdigit((unsigned char)line[10]) ||
            !isdigit((unsigned char)line[11]) ||
            line[12] != ' ') {
            return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
        }
        parser->status = (unsigned)(line[9] - '0') * 100u +
            (unsigned)(line[10] - '0') * 10u + (unsigned)(line[11] - '0');
        if (parser->status < 100u || parser->status > 599u) {
            return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
        }
        parser->version = maelys_http_internal_strdup("HTTP/1.1");
        parser->reason = maelys_http_internal_strndup(line + 13u,
                                                      length - 13u);
        if (!maelys_http_internal_header_value_valid(parser->reason,
                                     parser->reason ? strlen(parser->reason) : 0u)) {
            return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
        }
    }
    if (!parser->version ||
        (parser->kind == MAELYS_HTTP_PARSE_REQUEST &&
         (!parser->method || !parser->target)) ||
        (parser->kind == MAELYS_HTTP_PARSE_RESPONSE && !parser->reason)) {
        return fail(parser, MAELYS_HTTP_ERR_MEMORY);
    }
    parser->state = STATE_HEADERS;
    return MAELYS_HTTP_AGAIN;
}

static maelys_http_result_t append_header(
    maelys_http_parser_t *parser, int trailer,
    const char *name, size_t name_length,
    const char *value, size_t value_length) {
    owned_header_t **array = trailer ? &parser->trailers : &parser->headers;
    size_t *count = trailer ? &parser->trailer_count : &parser->header_count;
    size_t max_count = trailer ? parser->limits.max_trailer_count :
                                 parser->limits.max_header_count;
    size_t *bytes = trailer ? &parser->trailer_bytes : &parser->header_bytes;
    owned_header_t *resized;
    size_t index;
    (void)bytes;
    if (*count >= max_count || name_length > SIZE_MAX - value_length - 4u) {
        return fail(parser, MAELYS_HTTP_ERR_LIMIT);
    }
    if (!name_length) return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
    for (index = 0; index < name_length; ++index) {
        if (!maelys_http_internal_is_tchar((unsigned char)name[index])) {
            return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
        }
    }
    if (!maelys_http_internal_header_value_valid(value, value_length)) {
        return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
    }
    if (trailer && (maelys_http_internal_ascii_equal(name, name_length, "content-length") ||
                    maelys_http_internal_ascii_equal(name, name_length, "transfer-encoding") ||
                    maelys_http_internal_ascii_equal(name, name_length, "trailer"))) {
        return fail(parser, MAELYS_HTTP_ERR_FRAMING);
    }
    if (*count > SIZE_MAX / sizeof(**array) - 1u) {
        return fail(parser, MAELYS_HTTP_ERR_LIMIT);
    }
    resized = realloc(*array, (*count + 1u) * sizeof(**array));
    if (!resized) return fail(parser, MAELYS_HTTP_ERR_MEMORY);
    *array = resized;
    resized[*count].name = maelys_http_internal_strndup(name, name_length);
    resized[*count].value = maelys_http_internal_strndup(value, value_length);
    if (!resized[*count].name || !resized[*count].value) {
        free(resized[*count].name);
        free(resized[*count].value);
        resized[*count].name = NULL;
        resized[*count].value = NULL;
        return fail(parser, MAELYS_HTTP_ERR_MEMORY);
    }
    ++*count;
    return MAELYS_HTTP_AGAIN;
}

static maelys_http_result_t parse_header_line(
    maelys_http_parser_t *parser, int trailer) {
    char *colon;
    const char *value;
    size_t name_length;
    size_t value_length;
    if (parser->line_length == 0u) return MAELYS_HTTP_COMPLETE;
    /* Refuse obs-fold: a header line must never start with SP or HTAB. */
    if (parser->line[0] == ' ' || parser->line[0] == '\t') {
        return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
    }
    colon = memchr(parser->line, ':', parser->line_length);
    if (!colon) return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
    name_length = (size_t)(colon - parser->line);
    value = colon + 1;
    value_length = parser->line_length - name_length - 1u;
    while (value_length && (*value == ' ' || *value == '\t')) {
        ++value;
        --value_length;
    }
    while (value_length &&
           (value[value_length - 1u] == ' ' || value[value_length - 1u] == '\t')) {
        --value_length;
    }
    return append_header(parser, trailer, parser->line, name_length,
                         value, value_length);
}

static int parse_u64_decimal(const char *value, uint64_t *out) {
    uint64_t result = 0u;
    size_t index;
    size_t length = strlen(value);
    if (!length) return 0;
    for (index = 0; index < length; ++index) {
        unsigned digit;
        if (!isdigit((unsigned char)value[index])) return 0;
        digit = (unsigned)(value[index] - '0');
        if (result > (UINT64_MAX - digit) / 10u) return 0;
        result = result * 10u + digit;
    }
    *out = result;
    return 1;
}

static maelys_http_result_t headers_complete(maelys_http_parser_t *parser) {
    size_t index;
    int saw_length = 0;
    int saw_transfer = 0;
    int response_has_no_body;
    size_t host_count = 0u;
    uint64_t length = 0u;
    for (index = 0; index < parser->header_count; ++index) {
        owned_header_t *header = &parser->headers[index];
        if (parser->kind == MAELYS_HTTP_PARSE_REQUEST &&
            maelys_http_internal_ascii_equal(header->name, strlen(header->name),
                                             "host")) {
            ++host_count;
            if (host_count > 1u ||
                !maelys_http_internal_authority_valid(
                    header->value, strlen(header->value), 0)) {
                return fail(parser, MAELYS_HTTP_ERR_FRAMING);
            }
        }
        if (maelys_http_internal_ascii_equal(header->name, strlen(header->name), "content-length")) {
            if (saw_length || !parse_u64_decimal(header->value, &length)) {
                return fail(parser, MAELYS_HTTP_ERR_FRAMING);
            }
            saw_length = 1;
        } else if (maelys_http_internal_ascii_equal(header->name, strlen(header->name),
                                    "transfer-encoding")) {
            if (saw_transfer || !maelys_http_internal_ascii_equal(
                    header->value, strlen(header->value), "chunked")) {
                return fail(parser, MAELYS_HTTP_ERR_FRAMING);
            }
            saw_transfer = 1;
        }
    }
    if (parser->kind == MAELYS_HTTP_PARSE_REQUEST && host_count != 1u) {
        return fail(parser, MAELYS_HTTP_ERR_FRAMING);
    }
    if (saw_length && saw_transfer) return fail(parser, MAELYS_HTTP_ERR_FRAMING);
    response_has_no_body = parser->kind == MAELYS_HTTP_PARSE_RESPONSE &&
        (parser->response_to_head || parser->status / 100u == 1u ||
         parser->status == 204u || parser->status == 304u ||
         (parser->response_to_connect && parser->status / 100u == 2u));
    if (saw_length && !response_has_no_body &&
        length > parser->limits.max_body_bytes) {
        return fail(parser, MAELYS_HTTP_ERR_LIMIT);
    }
    parser->headers_complete = 1;
    if (response_has_no_body) {
        parser->framing = MAELYS_HTTP_BODY_NONE;
        parser->state = STATE_COMPLETE;
        parser->result = MAELYS_HTTP_COMPLETE;
    } else if (saw_transfer) {
        parser->framing = MAELYS_HTTP_BODY_CHUNKED;
        parser->state = STATE_CHUNK_SIZE;
    } else if (saw_length) {
        parser->framing = MAELYS_HTTP_BODY_CONTENT_LENGTH;
        parser->content_length = length;
        parser->remaining = length;
        parser->state = length ? STATE_FIXED : STATE_COMPLETE;
        if (!length) parser->result = MAELYS_HTTP_COMPLETE;
    } else if (parser->kind == MAELYS_HTTP_PARSE_RESPONSE) {
        parser->framing = MAELYS_HTTP_BODY_UNTIL_EOF;
        parser->state = STATE_UNTIL_EOF;
    } else {
        parser->framing = MAELYS_HTTP_BODY_NONE;
        parser->state = STATE_COMPLETE;
        parser->result = MAELYS_HTTP_COMPLETE;
    }
    return parser->state == STATE_COMPLETE ? MAELYS_HTTP_COMPLETE : MAELYS_HTTP_AGAIN;
}

static maelys_http_result_t parse_chunk_size(maelys_http_parser_t *parser) {
    uint64_t size = 0u;
    size_t index = 0u;
    int digits = 0;
    while (index < parser->line_length && parser->line[index] != ';') {
        unsigned char byte = (unsigned char)parser->line[index++];
        unsigned value;
        if (byte >= '0' && byte <= '9') value = (unsigned)(byte - '0');
        else if (byte >= 'a' && byte <= 'f') value = (unsigned)(byte - 'a') + 10u;
        else if (byte >= 'A' && byte <= 'F') value = (unsigned)(byte - 'A') + 10u;
        else return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
        if (size > (UINT64_MAX - value) / 16u) return fail(parser, MAELYS_HTTP_ERR_LIMIT);
        size = size * 16u + value;
        digits = 1;
    }
    if (!digits) return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
    while (index < parser->line_length) {
        size_t token_start;
        if (parser->line[index++] != ';') {
            return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
        }
        while (index < parser->line_length &&
               (parser->line[index] == ' ' || parser->line[index] == '\t')) ++index;
        token_start = index;
        while (index < parser->line_length &&
               maelys_http_internal_is_tchar((unsigned char)parser->line[index])) ++index;
        if (index == token_start) return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
        while (index < parser->line_length &&
               (parser->line[index] == ' ' || parser->line[index] == '\t')) ++index;
        if (index < parser->line_length && parser->line[index] == '=') {
            ++index;
            while (index < parser->line_length &&
                   (parser->line[index] == ' ' || parser->line[index] == '\t')) ++index;
            if (index < parser->line_length && parser->line[index] == '"') {
                int closed = 0;
                ++index;
                while (index < parser->line_length) {
                    unsigned char byte = (unsigned char)parser->line[index++];
                    if (byte == '"') {
                        closed = 1;
                        break;
                    }
                    if (byte == '\\') {
                        if (index == parser->line_length) {
                            return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
                        }
                        byte = (unsigned char)parser->line[index++];
                        if (!(byte == '\t' || byte == ' ' ||
                              (byte >= 0x21u && byte <= 0x7eu) ||
                              byte >= 0x80u)) {
                            return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
                        }
                    } else if (byte < 0x20u && byte != '\t') {
                        return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
                    } else if (byte == 0x7fu) {
                        return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
                    }
                }
                if (!closed) return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
            } else {
                token_start = index;
                while (index < parser->line_length &&
                       maelys_http_internal_is_tchar((unsigned char)parser->line[index])) ++index;
                if (index == token_start) return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
            }
            while (index < parser->line_length &&
                   (parser->line[index] == ' ' || parser->line[index] == '\t')) ++index;
        }
        if (index < parser->line_length && parser->line[index] != ';') {
            return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
        }
    }
    if (size > parser->limits.max_body_bytes - parser->body_bytes) {
        return fail(parser, MAELYS_HTTP_ERR_LIMIT);
    }
    parser->chunk_remaining = size;
    parser->state = size ? STATE_CHUNK_DATA : STATE_TRAILERS;
    return MAELYS_HTTP_AGAIN;
}

static maelys_http_result_t process_line(maelys_http_parser_t *parser) {
    maelys_http_result_t result;
    if (parser->state == STATE_START) return parse_start_line(parser);
    if (parser->state == STATE_HEADERS) {
        result = parse_header_line(parser, 0);
        if (result == MAELYS_HTTP_COMPLETE) return headers_complete(parser);
        return result;
    }
    if (parser->state == STATE_CHUNK_SIZE) return parse_chunk_size(parser);
    if (parser->state == STATE_TRAILERS) {
        result = parse_header_line(parser, 1);
        if (result == MAELYS_HTTP_COMPLETE) {
            parser->state = STATE_COMPLETE;
            parser->result = MAELYS_HTTP_COMPLETE;
            return MAELYS_HTTP_COMPLETE;
        }
        return result;
    }
    return fail(parser, MAELYS_HTTP_ERR_STATE);
}

static size_t current_line_limit(const maelys_http_parser_t *parser) {
    if (parser->state == STATE_START) return parser->limits.max_start_line_bytes;
    if (parser->state == STATE_CHUNK_SIZE) return parser->limits.max_chunk_line_bytes;
    return parser->limits.max_header_line_bytes;
}

static maelys_http_result_t feed_line_byte(
    maelys_http_parser_t *parser, unsigned char byte) {
    size_t limit = current_line_limit(parser);
    size_t *block_bytes = NULL;
    size_t block_limit = 0u;
    char *resized;
    maelys_http_result_t result;
    if (parser->state == STATE_HEADERS) {
        block_bytes = &parser->header_bytes;
        block_limit = parser->limits.max_header_bytes;
    } else if (parser->state == STATE_TRAILERS) {
        block_bytes = &parser->trailer_bytes;
        block_limit = parser->limits.max_trailer_bytes;
    }
    if (block_bytes) {
        if (*block_bytes >= block_limit) return fail(parser, MAELYS_HTTP_ERR_LIMIT);
        ++*block_bytes;
    }
    if (byte == '\n') {
        if (!parser->line_length || parser->line[parser->line_length - 1u] != '\r') {
            return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
        }
        if (parser->line_length >= limit) {
            return fail(parser, MAELYS_HTTP_ERR_LIMIT);
        }
        --parser->line_length;
        result = process_line(parser);
        parser->line_length = 0u;
        return result;
    }
    if (parser->line_length && parser->line[parser->line_length - 1u] == '\r') {
        return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
    }
    if (byte == '\0' || (byte < 0x20u && byte != '\t' && byte != '\r') ||
        byte == 0x7fu) return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
    if (parser->line_length >= limit) return fail(parser, MAELYS_HTTP_ERR_LIMIT);
    if (parser->line_length + 1u >= parser->line_capacity) {
        size_t maximum = limit + 1u;
        size_t capacity;
        if (!parser->line_capacity) {
            capacity = maximum < 128u ? maximum : 128u;
        } else if (parser->line_capacity > maximum / 2u) {
            capacity = maximum;
        } else {
            capacity = parser->line_capacity * 2u;
        }
        resized = realloc(parser->line, capacity);
        if (!resized) return fail(parser, MAELYS_HTTP_ERR_MEMORY);
        parser->line = resized;
        parser->line_capacity = capacity;
    }
    parser->line[parser->line_length++] = (char)byte;
    return MAELYS_HTTP_AGAIN;
}

static maelys_http_result_t deliver_body(
    maelys_http_parser_t *parser, const unsigned char *bytes, size_t length) {
    maelys_http_result_t result;
    if (!length) return MAELYS_HTTP_OK;
    if (parser->body_bytes > parser->limits.max_body_bytes - length) {
        return fail(parser, MAELYS_HTTP_ERR_LIMIT);
    }
    if (parser->body_fn) {
        result = parser->body_fn(parser->body_context, bytes, length);
        if (result == MAELYS_HTTP_AGAIN) return result;
        if (result != MAELYS_HTTP_OK) {
            if (result == MAELYS_HTTP_COMPLETE) result = MAELYS_HTTP_ERR_STATE;
            return fail(parser, result);
        }
    }
    parser->body_bytes += length;
    return MAELYS_HTTP_OK;
}

maelys_http_result_t maelys_http_parser_feed(
    maelys_http_parser_t *parser, const void *opaque, size_t length,
    size_t *out_consumed) {
    const unsigned char *bytes = opaque;
    size_t consumed = 0u;
    if (out_consumed) *out_consumed = 0u;
    if (!parser || (!bytes && length) || !out_consumed) return MAELYS_HTTP_ERR_ARGUMENT;
    if (parser->state == STATE_ERROR || parser->state == STATE_COMPLETE) {
        return parser->result;
    }
    while (consumed < length) {
        maelys_http_result_t result;
        size_t amount;
        if (parser->state == STATE_START || parser->state == STATE_HEADERS ||
            parser->state == STATE_CHUNK_SIZE || parser->state == STATE_TRAILERS) {
            result = feed_line_byte(parser, bytes[consumed]);
            if (result == MAELYS_HTTP_AGAIN || result == MAELYS_HTTP_OK) {
                ++consumed;
                continue;
            }
            if (result == MAELYS_HTTP_COMPLETE) ++consumed;
            *out_consumed = consumed;
            return result;
        }
        if (parser->state == STATE_FIXED || parser->state == STATE_CHUNK_DATA ||
            parser->state == STATE_UNTIL_EOF) {
            uint64_t available = length - consumed;
            uint64_t wanted = parser->state == STATE_FIXED ? parser->remaining :
                parser->state == STATE_CHUNK_DATA ? parser->chunk_remaining : available;
            amount = (size_t)(wanted < available ? wanted : available);
            result = deliver_body(parser, bytes + consumed, amount);
            if (result == MAELYS_HTTP_AGAIN) {
                *out_consumed = consumed;
                return result;
            }
            if (result != MAELYS_HTTP_OK) {
                *out_consumed = consumed;
                return result;
            }
            consumed += amount;
            if (parser->state == STATE_FIXED) {
                parser->remaining -= amount;
                if (!parser->remaining) {
                    parser->state = STATE_COMPLETE;
                    parser->result = MAELYS_HTTP_COMPLETE;
                    *out_consumed = consumed;
                    return MAELYS_HTTP_COMPLETE;
                }
            } else if (parser->state == STATE_CHUNK_DATA) {
                parser->chunk_remaining -= amount;
                if (!parser->chunk_remaining) parser->state = STATE_CHUNK_CR;
            }
            continue;
        }
        if (parser->state == STATE_CHUNK_CR) {
            if (bytes[consumed++] != '\r') {
                *out_consumed = consumed;
                return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
            }
            parser->state = STATE_CHUNK_LF;
            continue;
        }
        if (parser->state == STATE_CHUNK_LF) {
            if (bytes[consumed++] != '\n') {
                *out_consumed = consumed;
                return fail(parser, MAELYS_HTTP_ERR_SYNTAX);
            }
            parser->state = STATE_CHUNK_SIZE;
            continue;
        }
        *out_consumed = consumed;
        return fail(parser, MAELYS_HTTP_ERR_STATE);
    }
    *out_consumed = consumed;
    return parser->state == STATE_COMPLETE ? MAELYS_HTTP_COMPLETE : MAELYS_HTTP_AGAIN;
}

maelys_http_result_t maelys_http_parser_eof(maelys_http_parser_t *parser) {
    if (!parser) return MAELYS_HTTP_ERR_ARGUMENT;
    if (parser->state == STATE_UNTIL_EOF) {
        parser->state = STATE_COMPLETE;
        parser->result = MAELYS_HTTP_COMPLETE;
        return MAELYS_HTTP_COMPLETE;
    }
    if (parser->state == STATE_COMPLETE) return MAELYS_HTTP_COMPLETE;
    if (parser->state == STATE_ERROR) return parser->result;
    return fail(parser, MAELYS_HTTP_ERR_FRAMING);
}

maelys_http_result_t maelys_http_parser_reset(maelys_http_parser_t *parser) {
    if (!parser) return MAELYS_HTTP_ERR_ARGUMENT;
    clear_message(parser);
    parser->state = STATE_START;
    parser->result = MAELYS_HTTP_AGAIN;
    parser->response_to_head = 0;
    parser->response_to_connect = 0;
    return MAELYS_HTTP_OK;
}

void maelys_http_parser_release(maelys_http_parser_t *parser) {
    if (!parser) return;
    clear_message(parser);
    free(parser->line);
    free(parser);
}

maelys_http_result_t maelys_http_parser_result(const maelys_http_parser_t *parser) {
    return parser ? parser->result : MAELYS_HTTP_ERR_ARGUMENT;
}
int maelys_http_parser_headers_complete(const maelys_http_parser_t *parser) {
    return parser ? parser->headers_complete : 0;
}
maelys_http_body_framing_t maelys_http_parser_body_framing(const maelys_http_parser_t *parser) {
    return parser ? parser->framing : MAELYS_HTTP_BODY_NONE;
}
uint64_t maelys_http_parser_content_length(const maelys_http_parser_t *parser) {
    return parser ? parser->content_length : 0u;
}
uint64_t maelys_http_parser_body_bytes(const maelys_http_parser_t *parser) {
    return parser ? parser->body_bytes : 0u;
}
static maelys_http_slice_t string_slice(const char *value) {
    maelys_http_slice_t slice = {value, value ? strlen(value) : 0u};
    return slice;
}
maelys_http_slice_t maelys_http_parser_method(const maelys_http_parser_t *parser) {
    return parser ? string_slice(parser->method) : empty_slice();
}
maelys_http_slice_t maelys_http_parser_target(const maelys_http_parser_t *parser) {
    return parser ? string_slice(parser->target) : empty_slice();
}
unsigned maelys_http_parser_status(const maelys_http_parser_t *parser) {
    return parser ? parser->status : 0u;
}
maelys_http_slice_t maelys_http_parser_reason(const maelys_http_parser_t *parser) {
    return parser ? string_slice(parser->reason) : empty_slice();
}
maelys_http_slice_t maelys_http_parser_version(const maelys_http_parser_t *parser) {
    return parser ? string_slice(parser->version) : empty_slice();
}
size_t maelys_http_parser_header_count(const maelys_http_parser_t *parser) {
    return parser ? parser->header_count : 0u;
}
static maelys_http_header_view_t view_header(const owned_header_t *header) {
    maelys_http_header_view_t view;
    view.name = string_slice(header->name);
    view.value = string_slice(header->value);
    return view;
}
maelys_http_header_view_t maelys_http_parser_header(
    const maelys_http_parser_t *parser, size_t index) {
    return parser && index < parser->header_count ?
        view_header(&parser->headers[index]) : empty_header();
}
size_t maelys_http_parser_trailer_count(const maelys_http_parser_t *parser) {
    return parser ? parser->trailer_count : 0u;
}
maelys_http_header_view_t maelys_http_parser_trailer(
    const maelys_http_parser_t *parser, size_t index) {
    return parser && index < parser->trailer_count ?
        view_header(&parser->trailers[index]) : empty_header();
}
