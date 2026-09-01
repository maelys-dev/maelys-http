#ifndef MAELYS_HTTP_CLIENT_H
#define MAELYS_HTTP_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "maelys/http.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAELYS_HTTP_CLIENT_ABI_VERSION 1u
#define MAELYS_HTTP_TRANSPORT_ABI_VERSION 1u

typedef struct maelys_http_client maelys_http_client_t;
typedef struct maelys_http_request maelys_http_request_t;
typedef struct maelys_http_exchange maelys_http_exchange_t;
typedef struct maelys_http_transport maelys_http_transport_t;

typedef enum maelys_http_source_step {
    MAELYS_HTTP_SOURCE_DATA = 0,
    MAELYS_HTTP_SOURCE_END,
    MAELYS_HTTP_SOURCE_PAUSE,
    MAELYS_HTTP_SOURCE_FAILED
} maelys_http_source_step_t;

typedef enum maelys_http_sink_step {
    MAELYS_HTTP_SINK_ACCEPT = 0,
    MAELYS_HTTP_SINK_PAUSE,
    MAELYS_HTTP_SINK_FAILED
} maelys_http_sink_step_t;

typedef maelys_http_source_step_t (*maelys_http_body_source_fn)(
    void *context,
    unsigned char *buffer,
    size_t capacity,
    size_t *out_length);
typedef maelys_http_sink_step_t (*maelys_http_body_sink_fn)(
    void *context,
    const unsigned char *bytes,
    size_t length);

typedef enum maelys_http_headers_step {
    MAELYS_HTTP_HEADERS_ACCEPT = 0,
    MAELYS_HTTP_HEADERS_PAUSE,
    MAELYS_HTTP_HEADERS_FAILED
} maelys_http_headers_step_t;

/*
 * Called after final response headers are available and before any body byte.
 * ACCEPT admits body delivery. PAUSE admits nothing and replays the callback
 * with the same borrowed views on a later advance; FAILED is terminal.
 * Redirect policy runs first: FOLLOW suppresses intermediate headers/body,
 * while DENY exposes that response through this callback and the body sink.
 */
typedef maelys_http_headers_step_t (*maelys_http_response_headers_fn)(
    void *context,
    const maelys_http_exchange_t *exchange);

/*
 * A source returns DATA with 1..capacity bytes, END with zero bytes, PAUSE
 * with zero bytes, or FAILED. A sink returns ACCEPT after consuming the whole
 * slice, PAUSE after consuming none (the identical slice is replayed), or
 * FAILED. Source completion and sink acceptance deliberately use distinct
 * enums so a sink cannot ambiguously terminate a protocol-framed response.
 */

typedef enum maelys_http_redirect_decision {
    MAELYS_HTTP_REDIRECT_DENY = 0,
    MAELYS_HTTP_REDIRECT_FOLLOW = 1
} maelys_http_redirect_decision_t;

typedef maelys_http_redirect_decision_t (*maelys_http_redirect_fn)(
    void *context,
    unsigned status,
    maelys_http_slice_t old_authority,
    maelys_http_slice_t new_scheme,
    maelys_http_slice_t new_authority,
    maelys_http_slice_t new_target,
    size_t redirect_index);

typedef struct maelys_http_client_limits {
    maelys_http_limits_t parser;
    size_t io_buffer_bytes;
    size_t max_redirects;
    size_t max_informational_responses;
    size_t max_progress_steps_per_advance;
    uint64_t max_wait_slice_ms;
    uint64_t max_request_body_bytes;
    size_t max_connection_reuses;
    uint64_t idle_connection_ttl_ms;
} maelys_http_client_limits_t;

void maelys_http_client_limits_default(
    maelys_http_client_limits_t *out_limits);

typedef struct maelys_http_transport_ops {
    unsigned abi_version;
    const char *name;
    maelys_http_result_t (*open)(
        void *context,
        const char *scheme,
        const char *authority,
        uint64_t deadline_ms,
        void **out_stream,
        char **out_error);
    maelys_http_io_step_t (*read)(
        void *context,
        void *stream,
        void *buffer,
        size_t capacity,
        size_t *out_read);
    maelys_http_io_step_t (*write)(
        void *context,
        void *stream,
        const void *buffer,
        size_t length,
        size_t *out_written);
    maelys_http_result_t (*wait)(
        void *context,
        void *stream,
        int want_read,
        int want_write,
        uint64_t deadline_ms);
    void (*cancel)(void *context, void *stream);
    void (*close)(void *context, void *stream);
    const char *(*last_error)(void *context, const void *stream);
    void (*stream_release)(void *context, void *stream);
} maelys_http_transport_ops_t;

/*
 * Provider ownership contract:
 * - open sets *out_stream to a non-NULL owned implementation only with OK;
 *   otherwise it returns an ERR_* terminal result and leaves *out_stream NULL;
 * - open itself is nonblocking: it only initiates resolution/connection/TLS
 *   and returns promptly. A not-yet-ready stream reports WANT_READ/WRITE from
 *   read/write, and makes bounded progress from wait(deadline);
 * - non-NULL *out_error is allocated with malloc-compatible storage and is
 *   transferred to the caller, which frees it; providers never return a
 *   borrowed diagnostic there;
 * - read/write are nonblocking and set their byte output to zero unless they
 *   return IO_COMPLETE; COMPLETE transfers 1..requested bytes;
 * - wait returns only OK or an ERR_* result and honors the absolute monotonic
 *   deadline, including INFINITE;
 * - cancel and close are owner-thread-confined and idempotent; close does not
 *   release the stream; stream_release is called exactly once and must accept
 *   an already closed stream;
 * - last_error is a borrowed, non-secret string valid until stream_release.
 */

/* The vtable and name are copied; context is retained until final release. */
maelys_http_result_t maelys_http_transport_create(
    const maelys_http_transport_ops_t *ops,
    void *context,
    void (*release_context)(void *context),
    maelys_http_transport_t **out_transport,
    char **out_error);
void maelys_http_transport_retain(maelys_http_transport_t *transport);
void maelys_http_transport_release(maelys_http_transport_t *transport);

maelys_http_result_t maelys_http_client_create(
    maelys_http_transport_t *transport,
    const maelys_http_client_limits_t *limits,
    maelys_http_client_t **out_client);
void maelys_http_client_release(maelys_http_client_t *client);

/*
 * Connection reuse is opt-in; by default every request carries
 * Connection: close and each exchange closes its connection. When enabled,
 * requests omit that header and a completed exchange may park at most ONE
 * idle connection on the client handle — a slot, not a pool. The reuse key
 * is the scheme plus the canonical authority (host lowercased, explicit
 * port normalized) over the client's single immutable transport, which
 * carries the TLS identity. Ownership of the parked stream moves out of the
 * slot before an exchange may use it, so two exchanges can never share a
 * connection. A connection is destroyed instead of parked unless the
 * response completed with self-delimiting framing (Content-Length or
 * chunked; never to-EOF), its Connection list does not name close, and no
 * bytes remain beyond the framed response; error, timeout, cancellation,
 * redirect and abandonment paths always destroy. A parked connection is
 * destroyed at the next attempt when the key mismatches, idle_connection_ttl_ms
 * elapsed, max_connection_reuses is exhausted, or the idle stream is no
 * longer quiet (readable bytes, EOF — including a TLS closure without
 * close_notify — or failure). Disabling destroys any parked connection.
 */
maelys_http_result_t maelys_http_client_set_connection_reuse(
    maelys_http_client_t *client, int enabled);

maelys_http_result_t maelys_http_request_config_create(
    const char *method,
    const char *scheme,
    const char *authority,
    const char *target,
    maelys_http_request_t **out_request);
maelys_http_result_t maelys_http_request_add_header(
    maelys_http_request_t *request,
    const char *name,
    const char *value);
maelys_http_result_t maelys_http_request_set_fixed_body(
    maelys_http_request_t *request,
    uint64_t content_length,
    maelys_http_body_source_fn source,
    void *source_context);
maelys_http_result_t maelys_http_request_set_chunked_body(
    maelys_http_request_t *request,
    maelys_http_body_source_fn source,
    void *source_context);
maelys_http_result_t maelys_http_request_set_response_sink(
    maelys_http_request_t *request,
    maelys_http_body_sink_fn sink,
    void *sink_context);
maelys_http_result_t maelys_http_request_set_response_headers(
    maelys_http_request_t *request,
    maelys_http_response_headers_fn headers,
    void *headers_context);
maelys_http_result_t maelys_http_request_set_redirect_policy(
    maelys_http_request_t *request,
    maelys_http_redirect_fn redirect,
    void *redirect_context);
void maelys_http_request_release(maelys_http_request_t *request);

/*
 * Exchange creation copies the request strings and headers but borrows the
 * callback contexts. client and those contexts must outlive the exchange.
 * Client, request, exchange, transport-operation and provider-operation calls
 * are owner-thread confined. Atomic retain/release may move an idle handle but
 * do not license concurrent operations or concurrent use of one provider
 * context. cancel is invoked by the owner between advance calls and is
 * idempotent; wait slicing guarantees that the owner regains control.
 */
maelys_http_result_t maelys_http_exchange_create(
    maelys_http_client_t *client,
    const maelys_http_request_t *request,
    uint64_t deadline_ms,
    maelys_http_exchange_t **out_exchange);
/*
 * advance performs at most max_progress_steps_per_advance bounded progress
 * steps. One parser step examines at most io_buffer_bytes; one transport step
 * may include the readiness wait required by a nonblocking operation. AGAIN
 * means either that the fairness budget was reached or that an application
 * source/sink requested backpressure. The application knows the latter from
 * its own callback state: call again immediately after a fairness yield, or
 * after the paused callback is ready. A transport wait is capped by
 * max_wait_slice_ms so the owner regains control and can cancel even when the
 * exchange deadline is infinite. COMPLETE makes response views available
 * until release.
 */
maelys_http_result_t maelys_http_exchange_advance(
    maelys_http_exchange_t *exchange);
void maelys_http_exchange_cancel(maelys_http_exchange_t *exchange);
unsigned maelys_http_exchange_status(const maelys_http_exchange_t *exchange);
size_t maelys_http_exchange_header_count(
    const maelys_http_exchange_t *exchange);
maelys_http_header_view_t maelys_http_exchange_header(
    const maelys_http_exchange_t *exchange,
    size_t index);
size_t maelys_http_exchange_trailer_count(
    const maelys_http_exchange_t *exchange);
maelys_http_header_view_t maelys_http_exchange_trailer(
    const maelys_http_exchange_t *exchange,
    size_t index);
const char *maelys_http_exchange_error(
    const maelys_http_exchange_t *exchange);
void maelys_http_exchange_release(maelys_http_exchange_t *exchange);

#ifdef __cplusplus
}
#endif

#endif
