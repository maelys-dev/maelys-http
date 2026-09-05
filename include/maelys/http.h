#ifndef MAELYS_HTTP_H
#define MAELYS_HTTP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAELYS_HTTP_ABI_VERSION 1u
#define MAELYS_HTTP_VERSION_MAJOR 0u
#define MAELYS_HTTP_VERSION_MINOR 1u
#define MAELYS_HTTP_VERSION_PATCH 4u

typedef enum maelys_http_result {
    MAELYS_HTTP_OK = 0,
    MAELYS_HTTP_AGAIN,
    MAELYS_HTTP_COMPLETE,
    MAELYS_HTTP_ERR_ARGUMENT,
    MAELYS_HTTP_ERR_MEMORY,
    MAELYS_HTTP_ERR_LIMIT,
    MAELYS_HTTP_ERR_SYNTAX,
    MAELYS_HTTP_ERR_FRAMING,
    MAELYS_HTTP_ERR_STATE,
    MAELYS_HTTP_ERR_IO,
    MAELYS_HTTP_ERR_TIMEOUT,
    MAELYS_HTTP_ERR_CANCELLED,
    MAELYS_HTTP_ERR_TLS
} maelys_http_result_t;

typedef enum maelys_http_io_step {
    MAELYS_HTTP_IO_COMPLETE = 0,
    MAELYS_HTTP_IO_WANT_READ,
    MAELYS_HTTP_IO_WANT_WRITE,
    MAELYS_HTTP_IO_CLOSED,
    MAELYS_HTTP_IO_FAILED
} maelys_http_io_step_t;

const char *maelys_http_result_string(maelys_http_result_t result);

typedef struct maelys_http_slice {
    const char *data;
    size_t length;
} maelys_http_slice_t;

typedef struct maelys_http_header_view {
    maelys_http_slice_t name;
    maelys_http_slice_t value;
} maelys_http_header_view_t;

typedef enum maelys_http_parser_kind {
    MAELYS_HTTP_PARSE_REQUEST = 1,
    MAELYS_HTTP_PARSE_RESPONSE = 2
} maelys_http_parser_kind_t;

typedef enum maelys_http_body_framing {
    MAELYS_HTTP_BODY_NONE = 0,
    MAELYS_HTTP_BODY_CONTENT_LENGTH,
    MAELYS_HTTP_BODY_CHUNKED,
    MAELYS_HTTP_BODY_UNTIL_EOF
} maelys_http_body_framing_t;

typedef struct maelys_http_limits {
    /* Line and block limits count the exact wire bytes, including CRLF. */
    size_t max_start_line_bytes;
    size_t max_header_line_bytes;
    size_t max_header_bytes;
    size_t max_header_count;
    uint64_t max_body_bytes;
    size_t max_chunk_line_bytes;
    size_t max_trailer_bytes;
    size_t max_trailer_count;
} maelys_http_limits_t;

void maelys_http_limits_default(maelys_http_limits_t *out_limits);

typedef struct maelys_http_parser maelys_http_parser_t;

typedef maelys_http_result_t (*maelys_http_body_fn)(
    void *context,
    const unsigned char *bytes,
    size_t length);

/*
 * Parsers copy start-lines and headers into bounded internal storage. Views
 * returned by the accessors are borrowed until reset/release. Body bytes are
 * borrowed only for the duration of body_fn. body_fn may return OK after
 * consuming the bytes or AGAIN without consuming them. On AGAIN, out_consumed
 * excludes that body suffix: the caller must preserve and resubmit exactly the
 * same unconsumed octets on a later feed. The parser does not copy or compare
 * that application-owned suffix. Any other callback result becomes a sticky
 * parser error (success-class results are mapped to ERR_STATE).
 */
maelys_http_result_t maelys_http_parser_create(
    maelys_http_parser_kind_t kind,
    const maelys_http_limits_t *limits,
    maelys_http_body_fn body_fn,
    void *body_context,
    maelys_http_parser_t **out_parser);
maelys_http_result_t maelys_http_parser_set_response_to_head(
    maelys_http_parser_t *parser,
    int response_to_head);
/*
 * Set before feeding a response to CONNECT. A successful (2xx) CONNECT ends
 * at the header boundary; tunnel octets remain unconsumed for the caller.
 */
maelys_http_result_t maelys_http_parser_set_response_to_connect(
    maelys_http_parser_t *parser,
    int response_to_connect);
maelys_http_result_t maelys_http_parser_feed(
    maelys_http_parser_t *parser,
    const void *bytes,
    size_t length,
    size_t *out_consumed);
maelys_http_result_t maelys_http_parser_eof(maelys_http_parser_t *parser);
maelys_http_result_t maelys_http_parser_reset(maelys_http_parser_t *parser);
void maelys_http_parser_release(maelys_http_parser_t *parser);

maelys_http_result_t maelys_http_parser_result(
    const maelys_http_parser_t *parser);
int maelys_http_parser_headers_complete(
    const maelys_http_parser_t *parser);
maelys_http_body_framing_t maelys_http_parser_body_framing(
    const maelys_http_parser_t *parser);
uint64_t maelys_http_parser_content_length(
    const maelys_http_parser_t *parser);
uint64_t maelys_http_parser_body_bytes(
    const maelys_http_parser_t *parser);

/* Request start-line views. Empty on a response parser or until its line ends. */
maelys_http_slice_t maelys_http_parser_method(
    const maelys_http_parser_t *parser);
maelys_http_slice_t maelys_http_parser_target(
    const maelys_http_parser_t *parser);
/* Response start-line fields. */
unsigned maelys_http_parser_status(const maelys_http_parser_t *parser);
maelys_http_slice_t maelys_http_parser_reason(
    const maelys_http_parser_t *parser);
maelys_http_slice_t maelys_http_parser_version(
    const maelys_http_parser_t *parser);

size_t maelys_http_parser_header_count(const maelys_http_parser_t *parser);
maelys_http_header_view_t maelys_http_parser_header(
    const maelys_http_parser_t *parser,
    size_t index);
size_t maelys_http_parser_trailer_count(const maelys_http_parser_t *parser);
maelys_http_header_view_t maelys_http_parser_trailer(
    const maelys_http_parser_t *parser,
    size_t index);

typedef struct maelys_http_message maelys_http_message_t;
typedef maelys_http_result_t (*maelys_http_sink_fn)(
    void *context,
    const void *bytes,
    size_t length,
    size_t *out_written);

/*
 * Writer sinks are synchronous. They return OK with 1..length bytes consumed,
 * or a terminal error. AGAIN and COMPLETE are invalid because these stateless
 * helpers cannot resume an already partially emitted message; use the client
 * exchange's buffered writer when transport backpressure is required.
 */

maelys_http_result_t maelys_http_request_create(
    const char *method,
    const char *target,
    maelys_http_message_t **out_message);
maelys_http_result_t maelys_http_response_create(
    unsigned status,
    const char *reason,
    maelys_http_message_t **out_message);
maelys_http_result_t maelys_http_message_add_header(
    maelys_http_message_t *message,
    const char *name,
    const char *value);
maelys_http_result_t maelys_http_message_set_content_length(
    maelys_http_message_t *message,
    uint64_t length);
maelys_http_result_t maelys_http_message_set_chunked(
    maelys_http_message_t *message);
maelys_http_result_t maelys_http_message_write_head(
    const maelys_http_message_t *message,
    maelys_http_sink_fn sink,
    void *sink_context);
void maelys_http_message_release(maelys_http_message_t *message);

maelys_http_result_t maelys_http_write_chunk(
    const void *bytes,
    size_t length,
    maelys_http_sink_fn sink,
    void *sink_context);
maelys_http_result_t maelys_http_write_chunk_end(
    const maelys_http_header_view_t *trailers,
    size_t trailer_count,
    maelys_http_sink_fn sink,
    void *sink_context);

#ifdef __cplusplus
}
#endif

#endif
