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

/*
 * Keep-alive test double: one scripted request/response tape shared by every
 * stream. requests[n] pairs responses[n] regardless of which connection
 * carries it; a redial abandons a partially served response, and reads gate
 * on the matching request head so an idle probe sees a quiet connection.
 */
typedef struct ka_context {
    const char *responses[4];
    size_t response_count;
    size_t opens;
    size_t releases;
    size_t request_cursor;
    size_t response_cursor;
    size_t serve_offset;
    char requests[4][2048];
    size_t request_lengths[4];
    int close_after[4];
    int server_closed;
} ka_context_t;

typedef struct ka_stream {
    ka_context_t *context;
    int closed;
} ka_stream_t;

static maelys_http_result_t ka_open(
    void *opaque, const char *scheme, const char *authority,
    uint64_t deadline, void **out_stream, char **out_error) {
    ka_context_t *context = opaque;
    ka_stream_t *stream;
    (void)scheme; (void)authority; (void)deadline;
    if (out_error) *out_error = NULL;
    stream = calloc(1u, sizeof(*stream));
    if (!stream) return MAELYS_HTTP_ERR_MEMORY;
    stream->context = context;
    ++context->opens;
    context->server_closed = 0;
    if (context->serve_offset) {
        ++context->response_cursor;
        context->serve_offset = 0u;
    }
    *out_stream = stream;
    return MAELYS_HTTP_OK;
}

static maelys_http_io_step_t ka_read(
    void *opaque, void *stream_opaque, void *buffer, size_t capacity,
    size_t *out_read) {
    ka_context_t *context = opaque;
    ka_stream_t *stream = stream_opaque;
    const char *response;
    size_t length;
    size_t amount;
    if (stream->closed) return MAELYS_HTTP_IO_FAILED;
    if (context->server_closed) return MAELYS_HTTP_IO_CLOSED;
    if (context->response_cursor >= context->response_count ||
        !strstr(context->requests[context->response_cursor], "\r\n\r\n")) {
        return MAELYS_HTTP_IO_WANT_READ;
    }
    response = context->responses[context->response_cursor];
    length = strlen(response);
    amount = length - context->serve_offset;
    if (amount > capacity) amount = capacity;
    if (amount > 7u) amount = 7u;
    memcpy(buffer, response + context->serve_offset, amount);
    context->serve_offset += amount;
    *out_read = amount;
    if (context->serve_offset == length) {
        if (context->close_after[context->response_cursor]) {
            context->server_closed = 1;
        }
        ++context->response_cursor;
        context->serve_offset = 0u;
    }
    return MAELYS_HTTP_IO_COMPLETE;
}

static maelys_http_io_step_t ka_write(
    void *opaque, void *stream_opaque, const void *buffer, size_t length,
    size_t *out_written) {
    ka_context_t *context = opaque;
    ka_stream_t *stream = stream_opaque;
    size_t *current;
    if (stream->closed || context->server_closed) return MAELYS_HTTP_IO_FAILED;
    /* These exchanges carry no request body, so a recorded head ending in
     * the empty line means the next write starts the next request. */
    if (context->request_cursor + 1u <
            sizeof(context->requests) / sizeof(context->requests[0]) &&
        strstr(context->requests[context->request_cursor], "\r\n\r\n")) {
        ++context->request_cursor;
    }
    current = &context->request_lengths[context->request_cursor];
    if (*current + length >= sizeof(context->requests[0])) {
        return MAELYS_HTTP_IO_FAILED;
    }
    memcpy(context->requests[context->request_cursor] + *current, buffer, length);
    *current += length;
    context->requests[context->request_cursor][*current] = '\0';
    *out_written = length;
    return MAELYS_HTTP_IO_COMPLETE;
}

static maelys_http_result_t ka_wait(
    void *opaque, void *stream, int read, int write, uint64_t deadline) {
    (void)opaque; (void)stream; (void)read; (void)write; (void)deadline;
    return MAELYS_HTTP_OK;
}
static void ka_cancel(void *context, void *stream) { (void)context; (void)stream; }
static void ka_close(void *context, void *opaque) {
    (void)context;
    ((ka_stream_t *)opaque)->closed = 1;
}
static const char *ka_error(void *context, const void *stream) {
    (void)context; (void)stream;
    return "keep-alive fake transport failure";
}
static void ka_release(void *opaque, void *stream) {
    ka_context_t *context = opaque;
    ++context->releases;
    free(stream);
}

static maelys_http_transport_t *make_ka_transport(ka_context_t *context) {
    maelys_http_transport_ops_t ops;
    maelys_http_transport_t *transport = NULL;
    memset(&ops, 0, sizeof(ops));
    ops.abi_version = MAELYS_HTTP_TRANSPORT_ABI_VERSION;
    ops.name = "keep-alive-fake";
    ops.open = ka_open;
    ops.read = ka_read;
    ops.write = ka_write;
    ops.wait = ka_wait;
    ops.cancel = ka_cancel;
    ops.close = ka_close;
    ops.last_error = ka_error;
    ops.stream_release = ka_release;
    if (maelys_http_transport_create(&ops, context, NULL,
                                     &transport, NULL) != MAELYS_HTTP_OK) {
        return NULL;
    }
    return transport;
}

static int ka_get(maelys_http_client_t *client, const char *authority,
                  unsigned expected_status, const char *expected_body) {
    maelys_http_request_t *request = NULL;
    maelys_http_exchange_t *exchange = NULL;
    capture_t capture = {{0}, 0u};
    CHECK(maelys_http_request_config_create("GET", "http", authority, "/",
                                            &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_set_response_sink(request, capture_sink, &capture) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_create(client, request, UINT64_MAX, &exchange) ==
          MAELYS_HTTP_OK);
    CHECK(run_to_completion(exchange) == 0);
    CHECK(maelys_http_exchange_status(exchange) == expected_status);
    CHECK(capture.length == strlen(expected_body) &&
          !memcmp(capture.bytes, expected_body, capture.length));
    maelys_http_exchange_release(exchange);
    maelys_http_request_release(request);
    return 0;
}

static int ka_get_expect_error(maelys_http_client_t *client,
                               const char *authority,
                               maelys_http_result_t expected) {
    maelys_http_request_t *request = NULL;
    maelys_http_exchange_t *exchange = NULL;
    maelys_http_result_t result = MAELYS_HTTP_AGAIN;
    size_t steps;
    CHECK(maelys_http_request_config_create("GET", "http", authority, "/",
                                            &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_create(client, request, UINT64_MAX, &exchange) ==
          MAELYS_HTTP_OK);
    for (steps = 0u; steps < 64u && result == MAELYS_HTTP_AGAIN; ++steps) {
        result = maelys_http_exchange_advance(exchange);
    }
    CHECK(result == expected);
    maelys_http_exchange_release(exchange);
    maelys_http_request_release(request);
    return 0;
}

static int test_reuse_disabled_by_default_sends_close(void) {
    ka_context_t context;
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    memset(&context, 0, sizeof(context));
    context.responses[0] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\none";
    context.responses[1] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ntwo";
    context.response_count = 2u;
    transport = make_ka_transport(&context);
    CHECK(transport != NULL);
    CHECK(maelys_http_client_create(transport, NULL, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_client_set_connection_reuse(NULL, 1) ==
          MAELYS_HTTP_ERR_ARGUMENT);
    CHECK(maelys_http_client_set_connection_reuse(client, 2) ==
          MAELYS_HTTP_ERR_ARGUMENT);
    CHECK(ka_get(client, "example.test", 200u, "one") == 0);
    CHECK(ka_get(client, "example.test", 200u, "two") == 0);
    CHECK(context.opens == 2u);
    CHECK(count_text(context.requests[0], "Connection: close\r\n") == 1u);
    CHECK(count_text(context.requests[1], "Connection: close\r\n") == 1u);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    CHECK(context.releases == context.opens);
    return 0;
}

static int test_reuse_limit_validation(void) {
    ka_context_t context;
    maelys_http_client_limits_t limits;
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    memset(&context, 0, sizeof(context));
    transport = make_ka_transport(&context);
    CHECK(transport != NULL);
    maelys_http_client_limits_default(&limits);
    limits.max_connection_reuses = 0u;
    CHECK(maelys_http_client_create(transport, &limits, &client) ==
          MAELYS_HTTP_ERR_ARGUMENT);
    limits.max_connection_reuses = 65537u;
    CHECK(maelys_http_client_create(transport, &limits, &client) ==
          MAELYS_HTTP_ERR_ARGUMENT);
    maelys_http_client_limits_default(&limits);
    limits.idle_connection_ttl_ms = 0u;
    CHECK(maelys_http_client_create(transport, &limits, &client) ==
          MAELYS_HTTP_ERR_ARGUMENT);
    limits.idle_connection_ttl_ms = 3600001u;
    CHECK(maelys_http_client_create(transport, &limits, &client) ==
          MAELYS_HTTP_ERR_ARGUMENT);
    CHECK(client == NULL);
    maelys_http_transport_release(transport);
    return 0;
}

static int test_reuse_single_connection(void) {
    ka_context_t context;
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    memset(&context, 0, sizeof(context));
    context.responses[0] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\none";
    context.responses[1] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                           "3\r\ntwo\r\n0\r\n\r\n";
    context.response_count = 2u;
    transport = make_ka_transport(&context);
    CHECK(transport != NULL);
    CHECK(maelys_http_client_create(transport, NULL, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_client_set_connection_reuse(client, 1) == MAELYS_HTTP_OK);
    CHECK(ka_get(client, "example.test", 200u, "one") == 0);
    /* Same origin under canonicalization: lowercased host, default port. */
    CHECK(ka_get(client, "EXAMPLE.test:80", 200u, "two") == 0);
    CHECK(context.opens == 1u);
    CHECK(count_text(context.requests[0], "Connection:") == 0u);
    CHECK(count_text(context.requests[1], "Connection:") == 0u);
    CHECK(strstr(context.requests[0], "Host: example.test\r\n") != NULL);
    CHECK(strstr(context.requests[1], "Host: EXAMPLE.test:80\r\n") != NULL);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    CHECK(context.releases == context.opens);
    return 0;
}

static int test_reuse_ipv6_default_port_canonicalization(void) {
    ka_context_t context;
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    memset(&context, 0, sizeof(context));
    context.responses[0] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\none";
    context.responses[1] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ntwo";
    context.response_count = 2u;
    transport = make_ka_transport(&context);
    CHECK(transport != NULL);
    CHECK(maelys_http_client_create(transport, NULL, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_client_set_connection_reuse(client, 1) == MAELYS_HTTP_OK);
    CHECK(ka_get(client, "[2001:DB8::1]", 200u, "one") == 0);
    CHECK(ka_get(client, "[2001:db8::1]:80", 200u, "two") == 0);
    CHECK(context.opens == 1u);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    CHECK(context.releases == context.opens);
    return 0;
}

static int test_reuse_honors_response_connection_close(void) {
    ka_context_t context;
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    memset(&context, 0, sizeof(context));
    context.responses[0] = "HTTP/1.1 200 OK\r\nConnection: keep-alive, CLOSE\r\n"
                           "Content-Length: 3\r\n\r\none";
    context.responses[1] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ntwo";
    context.response_count = 2u;
    context.close_after[0] = 1;
    transport = make_ka_transport(&context);
    CHECK(transport != NULL);
    CHECK(maelys_http_client_create(transport, NULL, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_client_set_connection_reuse(client, 1) == MAELYS_HTTP_OK);
    CHECK(ka_get(client, "example.test", 200u, "one") == 0);
    CHECK(ka_get(client, "example.test", 200u, "two") == 0);
    CHECK(context.opens == 2u);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    CHECK(context.releases == context.opens);
    return 0;
}

static int test_reuse_idle_peer_close_redials(void) {
    ka_context_t context;
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    memset(&context, 0, sizeof(context));
    context.responses[0] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\none";
    context.responses[1] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ntwo";
    context.response_count = 2u;
    /* The first response is self-delimiting and carries no close token, but
     * the peer closes immediately after it. The idle probe must discard the
     * parked stream before the second request writes a byte. */
    context.close_after[0] = 1;
    transport = make_ka_transport(&context);
    CHECK(transport != NULL);
    CHECK(maelys_http_client_create(transport, NULL, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_client_set_connection_reuse(client, 1) == MAELYS_HTTP_OK);
    CHECK(ka_get(client, "example.test", 200u, "one") == 0);
    CHECK(ka_get(client, "example.test", 200u, "two") == 0);
    CHECK(context.opens == 2u);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    CHECK(context.releases == context.opens);
    return 0;
}

static int test_reuse_excess_bytes_redials(void) {
    ka_context_t context;
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    memset(&context, 0, sizeof(context));
    context.responses[0] =
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\noneX";
    context.responses[1] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ntwo";
    context.response_count = 2u;
    transport = make_ka_transport(&context);
    CHECK(transport != NULL);
    CHECK(maelys_http_client_create(transport, NULL, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_client_set_connection_reuse(client, 1) == MAELYS_HTTP_OK);
    /* The framed response remains a successful exchange, but the stray byte
     * makes the physical connection unusable for a subsequent response. */
    CHECK(ka_get(client, "example.test", 200u, "one") == 0);
    CHECK(ka_get(client, "example.test", 200u, "two") == 0);
    CHECK(context.opens == 2u);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    CHECK(context.releases == context.opens);
    return 0;
}

static int test_reuse_idle_ttl_expiry_redials(void) {
    ka_context_t context;
    maelys_http_client_limits_t limits;
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    struct timespec delay = {0, 10000000L};
    memset(&context, 0, sizeof(context));
    context.responses[0] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\none";
    context.responses[1] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ntwo";
    context.response_count = 2u;
    transport = make_ka_transport(&context);
    CHECK(transport != NULL);
    maelys_http_client_limits_default(&limits);
    limits.idle_connection_ttl_ms = 1u;
    CHECK(maelys_http_client_create(transport, &limits, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_client_set_connection_reuse(client, 1) == MAELYS_HTTP_OK);
    CHECK(ka_get(client, "example.test", 200u, "one") == 0);
    (void)nanosleep(&delay, NULL);
    CHECK(ka_get(client, "example.test", 200u, "two") == 0);
    CHECK(context.opens == 2u);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    CHECK(context.releases == context.opens);
    return 0;
}

static int test_reuse_budget_exhaustion_redials(void) {
    ka_context_t context;
    maelys_http_client_limits_t limits;
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    memset(&context, 0, sizeof(context));
    context.responses[0] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\none";
    context.responses[1] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ntwo";
    context.responses[2] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nthree";
    context.response_count = 3u;
    transport = make_ka_transport(&context);
    CHECK(transport != NULL);
    maelys_http_client_limits_default(&limits);
    limits.max_connection_reuses = 1u;
    CHECK(maelys_http_client_create(transport, &limits, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_client_set_connection_reuse(client, 1) == MAELYS_HTTP_OK);
    CHECK(ka_get(client, "example.test", 200u, "one") == 0);
    CHECK(ka_get(client, "example.test", 200u, "two") == 0);
    CHECK(context.opens == 1u);
    CHECK(ka_get(client, "example.test", 200u, "three") == 0);
    CHECK(context.opens == 2u);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    CHECK(context.releases == context.opens);
    return 0;
}

static int test_reuse_framing_error_destroys_and_surfaces(void) {
    ka_context_t context;
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    memset(&context, 0, sizeof(context));
    context.responses[0] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n"
                           "Transfer-Encoding: chunked\r\n\r\n"
                           "3\r\none\r\n0\r\n\r\n";
    context.responses[1] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ntwo";
    context.response_count = 2u;
    transport = make_ka_transport(&context);
    CHECK(transport != NULL);
    CHECK(maelys_http_client_create(transport, NULL, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_client_set_connection_reuse(client, 1) == MAELYS_HTTP_OK);
    CHECK(ka_get_expect_error(client, "example.test",
                              MAELYS_HTTP_ERR_FRAMING) == 0);
    CHECK(ka_get(client, "example.test", 200u, "two") == 0);
    CHECK(context.opens == 2u);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    CHECK(context.releases == context.opens);
    return 0;
}

static maelys_http_sink_step_t always_pause_sink(
    void *opaque, const unsigned char *bytes, size_t length) {
    (void)opaque; (void)bytes; (void)length;
    return MAELYS_HTTP_SINK_PAUSE;
}

static maelys_http_sink_step_t failing_sink(
    void *opaque, const unsigned char *bytes, size_t length) {
    (void)opaque; (void)bytes; (void)length;
    return MAELYS_HTTP_SINK_FAILED;
}

static int test_reuse_unconsumed_body_destroys(void) {
    ka_context_t context;
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    maelys_http_request_t *request = NULL;
    maelys_http_exchange_t *exchange = NULL;
    size_t steps;
    memset(&context, 0, sizeof(context));
    context.responses[0] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst";
    context.responses[1] = "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nsecond";
    context.responses[2] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nthird";
    context.response_count = 3u;
    transport = make_ka_transport(&context);
    CHECK(transport != NULL);
    CHECK(maelys_http_client_create(transport, NULL, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_client_set_connection_reuse(client, 1) == MAELYS_HTTP_OK);
    /* Abandoned mid-body: released before the framed body was consumed. */
    CHECK(maelys_http_request_config_create("GET", "http", "example.test", "/",
                                            &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_set_response_sink(request, always_pause_sink,
                                                NULL) == MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_create(client, request, UINT64_MAX, &exchange) ==
          MAELYS_HTTP_OK);
    for (steps = 0u; steps < 8u; ++steps) {
        CHECK(maelys_http_exchange_advance(exchange) == MAELYS_HTTP_AGAIN);
    }
    maelys_http_exchange_release(exchange);
    exchange = NULL;
    maelys_http_request_release(request);
    request = NULL;
    /* Sink failure mid-body surfaces the error and destroys as well. */
    CHECK(maelys_http_request_config_create("GET", "http", "example.test", "/",
                                            &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_set_response_sink(request, failing_sink,
                                                NULL) == MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_create(client, request, UINT64_MAX, &exchange) ==
          MAELYS_HTTP_OK);
    {
        maelys_http_result_t result = MAELYS_HTTP_AGAIN;
        for (steps = 0u; steps < 64u && result == MAELYS_HTTP_AGAIN; ++steps) {
            result = maelys_http_exchange_advance(exchange);
        }
        CHECK(result == MAELYS_HTTP_ERR_IO);
    }
    maelys_http_exchange_release(exchange);
    maelys_http_request_release(request);
    CHECK(context.opens == 2u);
    CHECK(ka_get(client, "example.test", 200u, "third") == 0);
    CHECK(context.opens == 3u);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    CHECK(context.releases == context.opens);
    return 0;
}

static int test_reuse_redirect_other_authority_dials_fresh(void) {
    ka_context_t context;
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    maelys_http_request_t *request = NULL;
    maelys_http_exchange_t *exchange = NULL;
    capture_t capture = {{0}, 0u};
    memset(&context, 0, sizeof(context));
    context.responses[0] = "HTTP/1.1 302 Found\r\nContent-Length: 0\r\n"
                           "Location: http://cdn.test/blob\r\n\r\n";
    context.responses[1] = "HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nartifact";
    context.responses[2] = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nnext";
    context.response_count = 3u;
    transport = make_ka_transport(&context);
    CHECK(transport != NULL);
    CHECK(maelys_http_client_create(transport, NULL, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_client_set_connection_reuse(client, 1) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_config_create("GET", "http", "registry.test",
                                            "/blob", &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_add_header(request, "Authorization",
                                         "Bearer secret") == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_set_redirect_policy(request, follow_redirect,
                                                  NULL) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_set_response_sink(request, capture_sink,
                                                &capture) == MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_create(client, request, UINT64_MAX, &exchange) ==
          MAELYS_HTTP_OK);
    CHECK(run_to_completion(exchange) == 0);
    CHECK(capture.length == 8u && !memcmp(capture.bytes, "artifact", 8u));
    maelys_http_exchange_release(exchange);
    maelys_http_request_release(request);
    /* The cross-authority hop dialed fresh and stripped the credential. */
    CHECK(context.opens == 2u);
    CHECK(strstr(context.requests[0], "Authorization: Bearer secret") != NULL);
    CHECK(strstr(context.requests[1], "Authorization") == NULL);
    CHECK(strstr(context.requests[1], "Host: cdn.test\r\n") != NULL);
    CHECK(count_text(context.requests[0], "Connection:") == 0u);
    CHECK(count_text(context.requests[1], "Connection:") == 0u);
    /* The post-redirect connection parked under the new authority's key. */
    CHECK(ka_get(client, "cdn.test", 200u, "next") == 0);
    CHECK(context.opens == 2u);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    CHECK(context.releases == context.opens);
    return 0;
}

static int test_reuse_obs_fold_response_rejected(void) {
    ka_context_t context;
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    memset(&context, 0, sizeof(context));
    context.responses[0] = "HTTP/1.1 200 OK\r\nX: y\r\n folded\r\n"
                           "Content-Length: 3\r\n\r\none";
    context.responses[1] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ntwo";
    context.response_count = 2u;
    transport = make_ka_transport(&context);
    CHECK(transport != NULL);
    CHECK(maelys_http_client_create(transport, NULL, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_client_set_connection_reuse(client, 1) == MAELYS_HTTP_OK);
    CHECK(ka_get_expect_error(client, "example.test",
                              MAELYS_HTTP_ERR_SYNTAX) == 0);
    CHECK(ka_get(client, "example.test", 200u, "two") == 0);
    CHECK(context.opens == 2u);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    CHECK(context.releases == context.opens);
    return 0;
}

static int test_reuse_disable_discards_parked_connection(void) {
    ka_context_t context;
    maelys_http_transport_t *transport;
    maelys_http_client_t *client = NULL;
    memset(&context, 0, sizeof(context));
    context.responses[0] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\none";
    context.responses[1] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ntwo";
    context.response_count = 2u;
    transport = make_ka_transport(&context);
    CHECK(transport != NULL);
    CHECK(maelys_http_client_create(transport, NULL, &client) == MAELYS_HTTP_OK);
    CHECK(maelys_http_client_set_connection_reuse(client, 1) == MAELYS_HTTP_OK);
    CHECK(ka_get(client, "example.test", 200u, "one") == 0);
    CHECK(maelys_http_client_set_connection_reuse(client, 0) == MAELYS_HTTP_OK);
    CHECK(context.releases == 1u);
    CHECK(ka_get(client, "example.test", 200u, "two") == 0);
    CHECK(context.opens == 2u);
    CHECK(count_text(context.requests[1], "Connection: close\r\n") == 1u);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    CHECK(context.releases == context.opens);
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
    CHECK(test_reuse_disabled_by_default_sends_close() == 0);
    CHECK(test_reuse_limit_validation() == 0);
    CHECK(test_reuse_single_connection() == 0);
    CHECK(test_reuse_ipv6_default_port_canonicalization() == 0);
    CHECK(test_reuse_honors_response_connection_close() == 0);
    CHECK(test_reuse_idle_peer_close_redials() == 0);
    CHECK(test_reuse_excess_bytes_redials() == 0);
    CHECK(test_reuse_idle_ttl_expiry_redials() == 0);
    CHECK(test_reuse_budget_exhaustion_redials() == 0);
    CHECK(test_reuse_framing_error_destroys_and_surfaces() == 0);
    CHECK(test_reuse_unconsumed_body_destroys() == 0);
    CHECK(test_reuse_redirect_other_authority_dials_fresh() == 0);
    CHECK(test_reuse_obs_fold_response_rejected() == 0);
    CHECK(test_reuse_disable_discards_parked_connection() == 0);
    puts("test_client: ok");
    return 0;
}
