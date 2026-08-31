#include "maelys/http_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "maelys/sys/clock.h"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); return 1; \
} } while (0)

typedef struct fake_context {
    const char *responses[4];
    size_t response_count;
    size_t opens;
    char requests[4][4096];
    size_t request_lengths[4];
    int force_timeout;
    int sleep_on_wait;
    int early_response;
    int success_diagnostic;
} fake_context_t;

typedef struct fake_stream {
    fake_context_t *context;
    size_t index;
    size_t response_offset;
    int closed;
} fake_stream_t;

static maelys_http_result_t fake_open(
    void *opaque, const char *scheme, const char *authority,
    uint64_t deadline, void **out_stream, char **out_error) {
    fake_context_t *context = opaque;
    fake_stream_t *stream;
    (void)scheme; (void)authority; (void)deadline;
    if (out_error) *out_error = NULL;
    if (context->opens >= context->response_count) return MAELYS_HTTP_ERR_IO;
    stream = calloc(1u, sizeof(*stream));
    if (!stream) return MAELYS_HTTP_ERR_MEMORY;
    stream->context = context;
    stream->index = context->opens++;
    if (context->success_diagnostic && out_error) {
        *out_error = strdup("provider note on successful open");
        if (!*out_error) {
            free(stream);
            return MAELYS_HTTP_ERR_MEMORY;
        }
    }
    *out_stream = stream;
    return MAELYS_HTTP_OK;
}
static maelys_http_io_step_t fake_read(
    void *opaque, void *stream_opaque, void *buffer, size_t capacity,
    size_t *out_read) {
    fake_context_t *context = opaque;
    fake_stream_t *stream = stream_opaque;
    const char *response = context->responses[stream->index];
    const char *request = context->requests[stream->index];
    const char *head_end = strstr(request, "\r\n\r\n");
    size_t length = strlen(response);
    size_t amount;
    if (context->force_timeout) return MAELYS_HTTP_IO_WANT_READ;
    if (!context->early_response) {
        if (!head_end) return MAELYS_HTTP_IO_WANT_READ;
        if (strstr(request, "Transfer-Encoding: chunked\r\n") &&
            !strstr(head_end + 4u, "0\r\n\r\n")) return MAELYS_HTTP_IO_WANT_READ;
        {
            const char *content_length = strstr(request, "Content-Length: ");
            if (content_length) {
                unsigned long declared = strtoul(content_length + 16u, NULL, 10);
                size_t body_length = context->request_lengths[stream->index] -
                                     (size_t)(head_end + 4u - request);
                if (body_length < declared) return MAELYS_HTTP_IO_WANT_READ;
            }
        }
    }
    if (stream->response_offset == length) return MAELYS_HTTP_IO_CLOSED;
    amount = length - stream->response_offset;
    if (amount > capacity) amount = capacity;
    if (amount > 7u) amount = 7u;
    memcpy(buffer, response + stream->response_offset, amount);
    stream->response_offset += amount;
    *out_read = amount;
    return MAELYS_HTTP_IO_COMPLETE;
}
static maelys_http_io_step_t fake_write(
    void *opaque, void *stream_opaque, const void *buffer, size_t length,
    size_t *out_written) {
    fake_context_t *context = opaque;
    fake_stream_t *stream = stream_opaque;
    size_t amount = length > 5u ? 5u : length;
    size_t *current = &context->request_lengths[stream->index];
    if (*current + amount > sizeof(context->requests[0])) return MAELYS_HTTP_IO_FAILED;
    memcpy(context->requests[stream->index] + *current, buffer, amount);
    *current += amount;
    context->requests[stream->index][*current] = '\0';
    *out_written = amount;
    return MAELYS_HTTP_IO_COMPLETE;
}
static maelys_http_result_t fake_wait(
    void *opaque, void *stream, int read, int write, uint64_t deadline) {
    fake_context_t *context = opaque;
    (void)stream; (void)read; (void)write; (void)deadline;
    if (context->sleep_on_wait) {
        struct timespec delay = {0, 3000000L};
        (void)nanosleep(&delay, NULL);
    }
    return context->force_timeout ? MAELYS_HTTP_ERR_TIMEOUT : MAELYS_HTTP_OK;
}
static void fake_cancel(void *context, void *stream) { (void)context; (void)stream; }
static void fake_close(void *context, void *opaque) {
    (void)context;
    ((fake_stream_t *)opaque)->closed = 1;
}
static const char *fake_error(void *context, const void *stream) {
    (void)context; (void)stream;
    return "fake transport failure";
}
static void fake_release(void *context, void *stream) { (void)context; free(stream); }

static maelys_http_transport_t *make_transport(fake_context_t *context) {
    maelys_http_transport_ops_t ops;
    maelys_http_transport_t *transport = NULL;
    memset(&ops, 0, sizeof(ops));
    ops.abi_version = MAELYS_HTTP_TRANSPORT_ABI_VERSION;
    ops.name = "fake";
    ops.open = fake_open;
    ops.read = fake_read;
    ops.write = fake_write;
    ops.wait = fake_wait;
    ops.cancel = fake_cancel;
    ops.close = fake_close;
    ops.last_error = fake_error;
    ops.stream_release = fake_release;
    if (maelys_http_transport_create(&ops, context, NULL,
                                     &transport, NULL) != MAELYS_HTTP_OK) {
        return NULL;
    }
    return transport;
}

typedef struct capture { char bytes[4096]; size_t length; } capture_t;
static maelys_http_sink_step_t capture_sink(
    void *opaque, const unsigned char *bytes, size_t length) {
    capture_t *capture = opaque;
    if (capture->length + length > sizeof(capture->bytes)) return MAELYS_HTTP_SINK_FAILED;
    memcpy(capture->bytes + capture->length, bytes, length);
    capture->length += length;
    return MAELYS_HTTP_SINK_ACCEPT;
}

typedef struct pausing_capture {
    capture_t capture;
    int paused;
    unsigned char first[64];
    size_t first_length;
} pausing_capture_t;

static maelys_http_sink_step_t pausing_sink(
    void *opaque, const unsigned char *bytes, size_t length) {
    pausing_capture_t *capture = opaque;
    if (!capture->paused) {
        if (length > sizeof(capture->first)) return MAELYS_HTTP_SINK_FAILED;
        memcpy(capture->first, bytes, length);
        capture->first_length = length;
        capture->paused = 1;
        return MAELYS_HTTP_SINK_PAUSE;
    }
    if (length != capture->first_length ||
        memcmp(bytes, capture->first, length) != 0) return MAELYS_HTTP_SINK_FAILED;
    return capture_sink(&capture->capture, bytes, length);
}

typedef struct source { const char *bytes; size_t length; size_t offset; } source_t;
static maelys_http_source_step_t fixed_source(
    void *opaque, unsigned char *buffer, size_t capacity, size_t *out_length) {
    source_t *source = opaque;
    size_t amount;
    if (source->offset == source->length) {
        *out_length = 0u;
        return MAELYS_HTTP_SOURCE_END;
    }
    amount = source->length - source->offset;
    if (amount > capacity) amount = capacity;
    if (amount > 3u) amount = 3u;
    memcpy(buffer, source->bytes + source->offset, amount);
    source->offset += amount;
    *out_length = amount;
    return MAELYS_HTTP_SOURCE_DATA;
}

static maelys_http_redirect_decision_t deny_redirect(
    void *context, unsigned status, maelys_http_slice_t old_authority,
    maelys_http_slice_t scheme, maelys_http_slice_t authority,
    maelys_http_slice_t target, size_t index) {
    if (context) ++*(size_t *)context;
    (void)status; (void)old_authority; (void)scheme;
    (void)authority; (void)target; (void)index;
    return MAELYS_HTTP_REDIRECT_DENY;
}

static maelys_http_redirect_decision_t follow_redirect(
    void *context, unsigned status, maelys_http_slice_t old_authority,
    maelys_http_slice_t scheme, maelys_http_slice_t authority,
    maelys_http_slice_t target, size_t index) {
    (void)context; (void)status; (void)old_authority; (void)scheme;
    (void)authority; (void)target; (void)index;
    return MAELYS_HTTP_REDIRECT_FOLLOW;
}

typedef struct headers_observer {
    size_t calls;
    unsigned expected_status;
    int admitted;
} headers_observer_t;

static maelys_http_headers_step_t pause_then_accept_headers(
    void *opaque, const maelys_http_exchange_t *exchange) {
    headers_observer_t *observer = opaque;
    ++observer->calls;
    if (maelys_http_exchange_status(exchange) != observer->expected_status) {
        return MAELYS_HTTP_HEADERS_FAILED;
    }
    if (observer->calls == 1u) return MAELYS_HTTP_HEADERS_PAUSE;
    observer->admitted = 1;
    return MAELYS_HTTP_HEADERS_ACCEPT;
}

typedef struct redirect_observer {
    fake_context_t *transport;
    size_t opens_seen;
    int tuple_valid;
} redirect_observer_t;

static maelys_http_redirect_decision_t observe_and_follow_redirect(
    void *opaque, unsigned status, maelys_http_slice_t old_authority,
    maelys_http_slice_t scheme, maelys_http_slice_t authority,
    maelys_http_slice_t target, size_t index) {
    redirect_observer_t *observer = opaque;
    (void)old_authority;
    observer->opens_seen = observer->transport->opens;
    observer->tuple_valid = status == 302u && index == 1u &&
        scheme.length == 5u && !memcmp(scheme.data, "https", 5u) &&
        authority.length == 11u && !memcmp(authority.data, "cdn.example", 11u) &&
        target.length == 5u && !memcmp(target.data, "/blob", 5u);
    return MAELYS_HTTP_REDIRECT_FOLLOW;
}

static int run_to_completion(maelys_http_exchange_t *exchange) {
    size_t steps;
    for (steps = 0; steps < 64u; ++steps) {
        maelys_http_result_t result = maelys_http_exchange_advance(exchange);
        if (result == MAELYS_HTTP_COMPLETE) return 0;
        CHECK(result == MAELYS_HTTP_AGAIN);
    }
    return 1;
}

static size_t count_text(const char *text, const char *needle) {
    size_t count = 0u;
    size_t length = strlen(needle);
    while ((text = strstr(text, needle)) != NULL) {
        ++count;
        text += length;
    }
    return count;
}

static int test_post_chunked_response(void) {
    fake_context_t context = {{0}, 1u, 0u, {{0}}, {0}, 0, 0, 0, 0};
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    maelys_http_request_t *request = NULL;
    maelys_http_exchange_t *exchange = NULL;
    capture_t capture = {{0}, 0u};
    source_t source = {"request", 7u, 0u};
    context.responses[0] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                           "4\r\nresp\r\n4\r\nonse\r\n0\r\n\r\n";
    transport = make_transport(&context);
    CHECK(transport != NULL);
    CHECK(maelys_http_client_create(transport, NULL, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_config_create("POST", "https", "api.example",
                                            "/mcp", &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_add_header(request, "Content-Type", "application/json") ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_request_set_fixed_body(request, 7u, fixed_source, &source) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_request_set_response_sink(request, capture_sink, &capture) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_create(client, request, UINT64_MAX, &exchange) ==
          MAELYS_HTTP_OK);
    CHECK(run_to_completion(exchange) == 0);
    CHECK(maelys_http_exchange_status(exchange) == 200u);
    CHECK(capture.length == 8u && !memcmp(capture.bytes, "response", 8u));
    CHECK(strstr(context.requests[0], "Host: api.example\r\n") != NULL);
    CHECK(count_text(context.requests[0], "Host:") == 1u);
    CHECK(strstr(context.requests[0], "Content-Length: 7\r\n") != NULL);
    CHECK(strstr(context.requests[0], "\r\n\r\nrequest") != NULL);
    maelys_http_exchange_release(exchange);
    maelys_http_request_release(request);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    return 0;
}

static int test_cross_authority_redirect_strips_secrets(void) {
    fake_context_t context = {{0}, 2u, 0u, {{0}}, {0}, 0, 0, 0, 0};
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    maelys_http_request_t *request = NULL;
    maelys_http_exchange_t *exchange = NULL;
    capture_t capture = {{0}, 0u};
    redirect_observer_t observer = {&context, 0u, 0};
    context.responses[0] = "HTTP/1.1 302 Found\r\nContent-Length: 5\r\n"
                           "Location: HTTPS://cdn.example/blob\r\n\r\nmoved";
    context.responses[1] = "HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nartifact";
    transport = make_transport(&context);
    CHECK(maelys_http_client_create(transport, NULL, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_config_create("GET", "https", "registry.example",
                                            "/blob", &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_add_header(request, "Authorization", "Bearer secret") ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_request_add_header(request, "Cookie", "secret=yes") ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_request_set_redirect_policy(
              request, observe_and_follow_redirect, &observer) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_request_set_response_sink(request, capture_sink, &capture) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_create(client, request, UINT64_MAX, &exchange) ==
          MAELYS_HTTP_OK);
    CHECK(run_to_completion(exchange) == 0);
    CHECK(context.opens == 2u);
    CHECK(observer.opens_seen == 1u && observer.tuple_valid);
    CHECK(strstr(context.requests[0], "Authorization: Bearer secret") != NULL);
    CHECK(strstr(context.requests[1], "Authorization") == NULL);
    CHECK(strstr(context.requests[1], "Cookie") == NULL);
    CHECK(strstr(context.requests[1], "Host: cdn.example") != NULL);
    CHECK(capture.length == 8u && !memcmp(capture.bytes, "artifact", 8u));
    maelys_http_exchange_release(exchange);
    maelys_http_request_release(request);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    return 0;
}

static int test_head_303_remains_head(void) {
    fake_context_t context = {{0}, 2u, 0u, {{0}}, {0}, 0, 0, 0, 0};
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    maelys_http_request_t *request = NULL;
    maelys_http_exchange_t *exchange = NULL;
    capture_t capture = {{0}, 0u};
    context.responses[0] = "HTTP/1.1 303 See Other\r\nContent-Length: 0\r\n"
                           "Location: /next?return=https://example.com/\r\n\r\n";
    context.responses[1] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nbody!";
    transport = make_transport(&context);
    CHECK(maelys_http_client_create(transport, NULL, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_config_create("HEAD", "http", "localhost",
                                            "/", &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_set_redirect_policy(request, follow_redirect, NULL) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_request_set_response_sink(request, capture_sink, &capture) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_create(client, request, UINT64_MAX, &exchange) ==
          MAELYS_HTTP_OK);
    CHECK(run_to_completion(exchange) == 0);
    CHECK(context.opens == 2u);
    CHECK(strstr(context.requests[1],
                 "HEAD /next?return=https://example.com/ HTTP/1.1\r\n") ==
          context.requests[1]);
    CHECK(capture.length == 0u);
    maelys_http_exchange_release(exchange);
    maelys_http_request_release(request);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    return 0;
}

static int test_early_final_stops_upload(void) {
    fake_context_t context = {{0}, 1u, 0u, {{0}}, {0}, 0, 0, 1, 0};
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    maelys_http_request_t *request = NULL;
    maelys_http_exchange_t *exchange = NULL;
    source_t source = {"abcdefghijklmnopqrstuvwxyz", 26u, 0u};
    context.responses[0] = "HTTP/1.1 413 Payload Too Large\r\n"
                           "Content-Length: 0\r\n\r\n";
    transport = make_transport(&context);
    CHECK(maelys_http_client_create(transport, NULL, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_config_create("POST", "http", "localhost",
                                            "/", &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_set_fixed_body(request, 26u, fixed_source, &source) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_create(client, request, UINT64_MAX, &exchange) ==
          MAELYS_HTTP_OK);
    CHECK(run_to_completion(exchange) == 0);
    CHECK(maelys_http_exchange_status(exchange) == 413u);
    CHECK(source.offset == 0u);
    maelys_http_exchange_release(exchange);
    maelys_http_request_release(request);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    return 0;
}

static int test_informational_and_upgrade_limits(void) {
    fake_context_t context = {{0}, 1u, 0u, {{0}}, {0}, 0, 0, 0, 0};
    maelys_http_client_limits_t limits;
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    maelys_http_request_t *request = NULL;
    maelys_http_exchange_t *exchange = NULL;
    context.responses[0] = "HTTP/1.1 100 Continue\r\n\r\n"
                           "HTTP/1.1 103 Early Hints\r\nLink: </x>\r\n\r\n"
                           "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    transport = make_transport(&context);
    maelys_http_client_limits_default(&limits);
    limits.max_informational_responses = 1u;
    CHECK(maelys_http_client_create(transport, &limits, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_config_create("GET", "http", "localhost",
                                            "/", &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_add_header(request, "Host", "other") ==
          MAELYS_HTTP_ERR_FRAMING);
    CHECK(maelys_http_request_add_header(request, "Connection", "Upgrade") ==
          MAELYS_HTTP_ERR_FRAMING);
    CHECK(maelys_http_request_add_header(request, "Upgrade", "websocket") ==
          MAELYS_HTTP_ERR_FRAMING);
    {
        maelys_http_request_t *connect_request = NULL;
        CHECK(maelys_http_request_config_create("CONNECT", "https",
                                                "example.test", "/",
                                                &connect_request) ==
              MAELYS_HTTP_ERR_ARGUMENT);
    }
    CHECK(maelys_http_exchange_create(client, request, UINT64_MAX, &exchange) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_advance(exchange) == MAELYS_HTTP_ERR_LIMIT);
    maelys_http_exchange_release(exchange);
    maelys_http_client_release(client);

    context.opens = 0u;
    context.responses[0] = "HTTP/1.1 101 Switching Protocols\r\n"
                           "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n";
    CHECK(maelys_http_client_create(transport, NULL, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_create(client, request, UINT64_MAX, &exchange) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_advance(exchange) == MAELYS_HTTP_ERR_FRAMING);
    maelys_http_exchange_release(exchange);
    maelys_http_request_release(request);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    return 0;
}

static int test_truncated_cancel_timeout(void) {
    fake_context_t context = {{0}, 1u, 0u, {{0}}, {0}, 0, 0, 0, 0};
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    maelys_http_request_t *request = NULL;
    maelys_http_exchange_t *exchange = NULL;
    context.responses[0] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nxx";
    transport = make_transport(&context);
    CHECK(maelys_http_client_create(transport, NULL, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_config_create("GET", "http", "localhost",
                                            "/", &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_create(client, request, UINT64_MAX, &exchange) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_advance(exchange) == MAELYS_HTTP_ERR_FRAMING);
    maelys_http_exchange_release(exchange);

    context.opens = 0u;
    CHECK(maelys_http_exchange_create(client, request, UINT64_MAX, &exchange) ==
          MAELYS_HTTP_OK);
    maelys_http_exchange_cancel(exchange);
    CHECK(maelys_http_exchange_advance(exchange) == MAELYS_HTTP_ERR_CANCELLED);
    maelys_http_exchange_release(exchange);

    context.opens = 0u;
    context.force_timeout = 1;
    CHECK(maelys_http_exchange_create(client, request, 0u, &exchange) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_advance(exchange) == MAELYS_HTTP_ERR_TIMEOUT);
    maelys_http_exchange_release(exchange);
    maelys_http_request_release(request);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    return 0;
}

static int test_interim_response(void) {
    fake_context_t context = {{0}, 1u, 0u, {{0}}, {0}, 0, 0, 0, 0};
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    maelys_http_request_t *request = NULL;
    maelys_http_exchange_t *exchange = NULL;
    context.responses[0] = "HTTP/1.1 100 Continue\r\n\r\n"
                           "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    transport = make_transport(&context);
    CHECK(transport != NULL);
    CHECK(maelys_http_client_create(transport, NULL, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_config_create("POST", "https", "api.example",
                                            "/", &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_create(client, request, UINT64_MAX, &exchange) ==
          MAELYS_HTTP_OK);
    CHECK(run_to_completion(exchange) == 0);
    CHECK(maelys_http_exchange_status(exchange) == 200u);
    maelys_http_exchange_release(exchange);
    maelys_http_request_release(request);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    return 0;
}

static int test_backpressure_chunked_request_and_trailers(void) {
    fake_context_t context = {{0}, 1u, 0u, {{0}}, {0}, 0, 0, 0, 0};
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    maelys_http_request_t *request = NULL;
    maelys_http_exchange_t *exchange = NULL;
    source_t source = {"abc", 3u, 0u};
    pausing_capture_t capture = {{{0}, 0u}, 0, {0}, 0u};
    maelys_http_header_view_t trailer;
    context.responses[0] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3\r\nxyz\r\n0\r\nX-End: yes\r\n\r\n";
    transport = make_transport(&context);
    CHECK(maelys_http_client_create(transport, NULL, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_config_create("POST", "http", "localhost",
                                            "/", &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_set_chunked_body(request, fixed_source, &source) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_request_set_response_sink(request, pausing_sink, &capture) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_create(client, request, UINT64_MAX, &exchange) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_advance(exchange) == MAELYS_HTTP_AGAIN);
    CHECK(run_to_completion(exchange) == 0);
    CHECK(capture.capture.length == 3u &&
          !memcmp(capture.capture.bytes, "xyz", 3u));
    CHECK(strstr(context.requests[0], "Transfer-Encoding: chunked\r\n") != NULL);
    CHECK(strstr(context.requests[0], "3\r\nabc\r\n0\r\n\r\n") != NULL);
    CHECK(maelys_http_exchange_trailer_count(exchange) == 1u);
    trailer = maelys_http_exchange_trailer(exchange, 0u);
    CHECK(trailer.name.length == 5u && !memcmp(trailer.name.data, "X-End", 5u));
    maelys_http_exchange_release(exchange);
    maelys_http_request_release(request);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    return 0;
}

static int test_redirect_deny_precedes_body_replay_policy(void) {
    fake_context_t context = {{0}, 1u, 0u, {{0}}, {0}, 0, 0, 0, 0};
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    maelys_http_request_t *request = NULL;
    maelys_http_exchange_t *exchange = NULL;
    source_t source = {"body", 4u, 0u};
    capture_t capture = {{0}, 0u};
    headers_observer_t headers = {0u, 302u, 0};
    size_t redirect_calls = 0u;
    context.responses[0] = "HTTP/1.1 302 Found\r\nContent-Length: 5\r\n"
                           "Location: /other\r\n\r\nmoved";
    transport = make_transport(&context);
    CHECK(maelys_http_client_create(transport, NULL, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_config_create("POST", "http", "localhost",
                                            "/", &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_set_fixed_body(request, 4u, fixed_source, &source) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_request_set_redirect_policy(request, deny_redirect,
                                                  &redirect_calls) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_request_set_response_headers(
              request, pause_then_accept_headers, &headers) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_set_response_sink(request, capture_sink, &capture) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_create(client, request, UINT64_MAX, &exchange) ==
          MAELYS_HTTP_OK);
    CHECK(run_to_completion(exchange) == 0);
    CHECK(context.opens == 1u && maelys_http_exchange_status(exchange) == 302u);
    CHECK(redirect_calls == 1u && headers.calls == 2u && headers.admitted);
    CHECK(capture.length == 5u && !memcmp(capture.bytes, "moved", 5u));
    maelys_http_exchange_release(exchange);
    maelys_http_request_release(request);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    return 0;
}

static int test_wait_slice_cancel_and_late_deadline(void) {
    fake_context_t context = {{0}, 1u, 0u, {{0}}, {0}, 1, 0, 0, 0};
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    maelys_http_request_t *request = NULL;
    maelys_http_exchange_t *exchange = NULL;
    uint64_t deadline;
    context.responses[0] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    transport = make_transport(&context);
    CHECK(maelys_http_client_create(transport, NULL, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_config_create("GET", "http", "localhost",
                                            "/", &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_create(client, request, UINT64_MAX, &exchange) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_advance(exchange) == MAELYS_HTTP_AGAIN);
    maelys_http_exchange_cancel(exchange);
    maelys_http_exchange_cancel(exchange);
    CHECK(maelys_http_exchange_advance(exchange) == MAELYS_HTTP_ERR_CANCELLED);
    maelys_http_exchange_release(exchange);

    context.opens = 0u;
    context.sleep_on_wait = 1;
    CHECK(maelys_sys_deadline_after(1u, &deadline) == MAELYS_SYS_OK);
    CHECK(maelys_http_exchange_create(client, request, deadline, &exchange) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_advance(exchange) == MAELYS_HTTP_ERR_TIMEOUT);
    maelys_http_exchange_release(exchange);
    maelys_http_request_release(request);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    return 0;
}

static int test_minimum_fairness_budget_and_authority(void) {
    fake_context_t context = {{0}, 1u, 0u, {{0}}, {0}, 0, 0, 0, 0};
    maelys_http_client_limits_t limits;
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    maelys_http_request_t *request = NULL;
    maelys_http_exchange_t *exchange = NULL;
    source_t source = {"abc", 3u, 0u};
    size_t steps;
    context.responses[0] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    transport = make_transport(&context);
    maelys_http_client_limits_default(&limits);
    limits.max_progress_steps_per_advance = 1u;
    CHECK(maelys_http_client_create(transport, &limits, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_config_create("GET", "http", "::1",
                                            "/", &request) == MAELYS_HTTP_ERR_ARGUMENT);
    CHECK(maelys_http_request_config_create("GET", "http", "[example.com]",
                                            "/", &request) == MAELYS_HTTP_ERR_ARGUMENT);
    CHECK(maelys_http_request_config_create("GET", "http", "[127.0.0.1]",
                                            "/", &request) == MAELYS_HTTP_ERR_ARGUMENT);
    CHECK(maelys_http_request_config_create("GET", "http", "exa[mple]",
                                            "/", &request) == MAELYS_HTTP_ERR_ARGUMENT);
    CHECK(maelys_http_request_config_create("GET", "http", "bad%ZZ",
                                            "/", &request) == MAELYS_HTTP_ERR_ARGUMENT);
    CHECK(maelys_http_request_config_create("GET", "http", "ex%61mple.com",
                                            "/", &request) == MAELYS_HTTP_ERR_ARGUMENT);
    CHECK(maelys_http_request_config_create("GET", "http", "localhost",
                                            "/bad%ZZ", &request) ==
          MAELYS_HTTP_ERR_ARGUMENT);
    CHECK(maelys_http_request_config_create("GET", "http", "[::1]",
                                            "/", &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_create(client, request, UINT64_MAX, &exchange) ==
          MAELYS_HTTP_OK);
    for (steps = 0u; steps < 256u; ++steps) {
        maelys_http_result_t result = maelys_http_exchange_advance(exchange);
        if (result == MAELYS_HTTP_COMPLETE) break;
        CHECK(result == MAELYS_HTTP_AGAIN);
    }
    CHECK(steps < 256u);
    maelys_http_exchange_cancel(exchange);
    CHECK(maelys_http_exchange_advance(exchange) == MAELYS_HTTP_COMPLETE);
    maelys_http_exchange_release(exchange);
    maelys_http_request_release(request);

    context.opens = 0u;
    memset(context.requests, 0, sizeof(context.requests));
    memset(context.request_lengths, 0, sizeof(context.request_lengths));
    CHECK(maelys_http_request_config_create("POST", "http", "localhost",
                                            "/", &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_set_fixed_body(request, 3u, fixed_source, &source) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_create(client, request, UINT64_MAX, &exchange) ==
          MAELYS_HTTP_OK);
    for (steps = 0u; steps < 256u; ++steps) {
        maelys_http_result_t result = maelys_http_exchange_advance(exchange);
        if (result == MAELYS_HTTP_COMPLETE) break;
        CHECK(result == MAELYS_HTTP_AGAIN);
    }
    CHECK(steps < 256u && strstr(context.requests[0], "\r\n\r\nabc") != NULL);
    maelys_http_exchange_release(exchange);
    maelys_http_request_release(request);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    return 0;
}

static int exchange_creation_is_limited(
    maelys_http_client_t *client, const maelys_http_request_t *request) {
    maelys_http_exchange_t *exchange = NULL;
    maelys_http_result_t result = maelys_http_exchange_create(
        client, request, UINT64_MAX, &exchange);
    maelys_http_exchange_release(exchange);
    return result == MAELYS_HTTP_ERR_LIMIT;
}

static int test_outgoing_head_limits_before_open(void) {
    fake_context_t context = {{0}, 1u, 0u, {{0}}, {0}, 0, 0, 0, 0};
    maelys_http_client_limits_t limits;
    maelys_http_transport_t *transport = make_transport(&context);
    maelys_http_client_t *client = NULL;
    maelys_http_request_t *request = NULL;
    maelys_http_exchange_t *exchange = NULL;
    char *large = NULL;
    size_t index;
    context.responses[0] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";

    maelys_http_client_limits_default(&limits);
    limits.parser.max_start_line_bytes = 16u;
    limits.parser.max_header_line_bytes = 19u;
    limits.parser.max_header_bytes = 30u;
    limits.parser.max_header_count = 2u;
    CHECK(maelys_http_client_create(transport, &limits, &client) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_request_config_create(
              "GET", "http", "x", "/", &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_create(client, request, UINT64_MAX, &exchange) ==
          MAELYS_HTTP_OK);
    CHECK(context.opens == 0u);
    maelys_http_exchange_release(exchange);
    maelys_http_request_release(request);
    maelys_http_client_release(client);

    maelys_http_client_limits_default(&limits);
    CHECK(maelys_http_client_create(transport, &limits, &client) ==
          MAELYS_HTTP_OK);
    large = malloc(9002u);
    CHECK(large != NULL);
    large[0] = '/';
    memset(large + 1u, 'a', 9000u);
    large[9001u] = '\0';
    CHECK(maelys_http_request_config_create(
              "GET", "http", "x", large, &request) == MAELYS_HTTP_OK);
    CHECK(exchange_creation_is_limited(client, request));
    maelys_http_request_release(request);
    request = NULL;

    memset(large, 'b', 9000u);
    large[9000u] = '\0';
    CHECK(maelys_http_request_config_create(
              "GET", "http", "x", "/", &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_add_header(request, "X-Large", large) ==
          MAELYS_HTTP_OK);
    CHECK(exchange_creation_is_limited(client, request));
    maelys_http_request_release(request);
    request = NULL;

    memset(large, 'c', 8000u);
    large[8000u] = '\0';
    CHECK(maelys_http_request_config_create(
              "GET", "http", "x", "/", &request) == MAELYS_HTTP_OK);
    for (index = 0u; index < 9u; ++index) {
        CHECK(maelys_http_request_add_header(request, "X-Block", large) ==
              MAELYS_HTTP_OK);
    }
    CHECK(exchange_creation_is_limited(client, request));
    maelys_http_request_release(request);
    request = NULL;

    CHECK(maelys_http_request_config_create(
              "GET", "http", "x", "/", &request) == MAELYS_HTTP_OK);
    for (index = 0u; index < 127u; ++index) {
        CHECK(maelys_http_request_add_header(request, "X", "y") ==
              MAELYS_HTTP_OK);
    }
    CHECK(exchange_creation_is_limited(client, request));
    CHECK(context.opens == 0u);
    maelys_http_request_release(request);
    free(large);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    return 0;
}

static int test_success_open_diagnostic_is_owned(void) {
    fake_context_t context = {{0}, 1u, 0u, {{0}}, {0}, 0, 0, 0, 1};
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    maelys_http_request_t *request = NULL;
    maelys_http_exchange_t *exchange = NULL;
    context.responses[0] = "HTTP/1.1 204 No Content\r\n\r\n";
    transport = make_transport(&context);
    CHECK(maelys_http_client_create(transport, NULL, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_config_create(
              "GET", "http", "localhost", "/", &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_create(client, request, UINT64_MAX, &exchange) ==
          MAELYS_HTTP_OK);
    CHECK(run_to_completion(exchange) == 0 && context.opens == 1u);
    maelys_http_exchange_release(exchange);
    maelys_http_request_release(request);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    return 0;
}

static int test_redirect_head_limit_precedes_second_open(void) {
    fake_context_t context = {{0}, 1u, 0u, {{0}}, {0}, 0, 0, 0, 0};
    maelys_http_client_limits_t limits;
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    maelys_http_request_t *request = NULL;
    maelys_http_exchange_t *exchange = NULL;
    char response[1024];
    char location[257];
    maelys_http_result_t result = MAELYS_HTTP_AGAIN;
    size_t steps;
    location[0] = '/';
    memset(location + 1u, 'a', sizeof(location) - 2u);
    location[sizeof(location) - 1u] = '\0';
    CHECK(snprintf(response, sizeof(response),
                   "HTTP/1.1 302 Found\r\nContent-Length: 0\r\n"
                   "Location: %s\r\n\r\n", location) > 0);
    context.responses[0] = response;
    transport = make_transport(&context);
    maelys_http_client_limits_default(&limits);
    limits.parser.max_start_line_bytes = 64u;
    CHECK(maelys_http_client_create(transport, &limits, &client) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_request_config_create(
              "GET", "http", "first.test", "/", &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_set_redirect_policy(
              request, follow_redirect, NULL) == MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_create(client, request, UINT64_MAX, &exchange) ==
          MAELYS_HTTP_OK);
    for (steps = 0u; steps < 256u && result == MAELYS_HTTP_AGAIN; ++steps) {
        result = maelys_http_exchange_advance(exchange);
    }
    CHECK(result == MAELYS_HTTP_ERR_LIMIT);
    CHECK(context.opens == 1u);
    maelys_http_exchange_release(exchange);
    maelys_http_request_release(request);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    return 0;
}

int main(void) {
    CHECK(test_post_chunked_response() == 0);
    CHECK(test_cross_authority_redirect_strips_secrets() == 0);
    CHECK(test_head_303_remains_head() == 0);
    CHECK(test_early_final_stops_upload() == 0);
    CHECK(test_truncated_cancel_timeout() == 0);
    CHECK(test_interim_response() == 0);
    CHECK(test_backpressure_chunked_request_and_trailers() == 0);
    CHECK(test_redirect_deny_precedes_body_replay_policy() == 0);
    CHECK(test_wait_slice_cancel_and_late_deadline() == 0);
    CHECK(test_minimum_fairness_budget_and_authority() == 0);
    CHECK(test_outgoing_head_limits_before_open() == 0);
    CHECK(test_success_open_diagnostic_is_owned() == 0);
    CHECK(test_redirect_head_limit_precedes_second_open() == 0);
    CHECK(test_informational_and_upgrade_limits() == 0);
    puts("test_client: ok");
    return 0;
}
