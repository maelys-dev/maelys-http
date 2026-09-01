#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "maelys/sys/clock.h"

typedef enum exchange_state {
    EXCHANGE_OPEN = 0,
    EXCHANGE_SEND_HEAD,
    EXCHANGE_SEND_BODY,
    EXCHANGE_READ,
    EXCHANGE_DONE,
    EXCHANGE_FAILED
} exchange_state_t;

struct maelys_http_client {
    maelys_http_transport_t *transport;
    maelys_http_client_limits_t limits;
    int reuse_enabled;
    void *idle_stream;
    char *idle_key;
    size_t idle_reuse_count;
    uint64_t idle_expiry_ms;
};

struct maelys_http_request {
    char *method;
    char *scheme;
    char *authority;
    char *target;
    owned_header_t *headers;
    size_t header_count;
    int body_mode; /* 0 none, 1 fixed, 2 chunked */
    uint64_t content_length;
    maelys_http_body_source_fn source;
    void *source_context;
    maelys_http_body_sink_fn sink;
    void *sink_context;
    maelys_http_response_headers_fn response_headers;
    void *response_headers_context;
    maelys_http_redirect_fn redirect;
    void *redirect_context;
};

typedef struct byte_buffer {
    unsigned char *bytes;
    size_t length;
    size_t offset;
    size_t capacity;
} byte_buffer_t;

struct maelys_http_exchange {
    maelys_http_client_t *client;
    maelys_http_request_t request;
    uint64_t deadline_ms;
    exchange_state_t state;
    void *stream;
    maelys_http_parser_t *parser;
    byte_buffer_t outgoing;
    byte_buffer_t incoming;
    uint64_t request_body_sent;
    size_t redirect_count;
    size_t informational_count;
    size_t connection_reuses;
    int reuse_mode;
    int cancelled;
    int sink_paused;
    int upload_probe_due;
    int headers_pending;
    int headers_accepted;
    int redirect_checked;
    char *error;
};

static int valid_scheme(const char *scheme) {
    return scheme &&
        (maelys_http_internal_ascii_equal(scheme, strlen(scheme), "http") ||
         maelys_http_internal_ascii_equal(scheme, strlen(scheme), "https"));
}

static int valid_authority(const char *authority) {
    /* URI reg-name percent encoding is valid HTTP syntax, but getaddrinfo
     * consumes a decoded DNS name. H2 has no URI decoder, so reject rather
     * than resolving a different literal string. */
    return authority && !strchr(authority, '%') &&
        maelys_http_internal_authority_valid(
            authority, strlen(authority), 0);
}

void maelys_http_client_limits_default(maelys_http_client_limits_t *limits) {
    if (!limits) return;
    maelys_http_limits_default(&limits->parser);
    limits->io_buffer_bytes = 16384u;
    limits->max_redirects = 5u;
    limits->max_informational_responses = 8u;
    limits->max_progress_steps_per_advance = 64u;
    limits->max_wait_slice_ms = 50u;
    limits->max_request_body_bytes = UINT64_C(64) * 1024u * 1024u;
    limits->max_connection_reuses = 64u;
    limits->idle_connection_ttl_ms = 30000u;
}

maelys_http_result_t maelys_http_transport_create(
    const maelys_http_transport_ops_t *ops, void *context,
    void (*release_context)(void *context),
    maelys_http_transport_t **out_transport, char **out_error) {
    maelys_http_transport_t *transport;
    if (out_transport) *out_transport = NULL;
    if (out_error) *out_error = NULL;
    if (!ops || !out_transport ||
        ops->abi_version != MAELYS_HTTP_TRANSPORT_ABI_VERSION ||
        !ops->name || !ops->name[0] || !ops->open || !ops->read || !ops->write ||
        !ops->wait || !ops->cancel || !ops->close || !ops->last_error ||
        !ops->stream_release) {
        maelys_http_internal_set_error(out_error, "invalid HTTP transport ABI or operations");
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    transport = calloc(1u, sizeof(*transport));
    if (!transport) return MAELYS_HTTP_ERR_MEMORY;
    transport->name = maelys_http_internal_strdup(ops->name);
    if (!transport->name) {
        free(transport);
        return MAELYS_HTTP_ERR_MEMORY;
    }
    transport->ops = *ops;
    transport->ops.name = transport->name;
    transport->context = context;
    transport->release_context = release_context;
    atomic_init(&transport->references, 1u);
    *out_transport = transport;
    return MAELYS_HTTP_OK;
}

void maelys_http_transport_retain(maelys_http_transport_t *transport) {
    if (transport) (void)atomic_fetch_add_explicit(
        &transport->references, 1u, memory_order_relaxed);
}

void maelys_http_transport_release(maelys_http_transport_t *transport) {
    if (!transport || atomic_fetch_sub_explicit(
            &transport->references, 1u, memory_order_acq_rel) != 1u) return;
    if (transport->release_context) transport->release_context(transport->context);
    free(transport->name);
    free(transport);
}

maelys_http_result_t maelys_http_client_create(
    maelys_http_transport_t *transport,
    const maelys_http_client_limits_t *limits,
    maelys_http_client_t **out_client) {
    maelys_http_client_limits_t defaults;
    maelys_http_client_t *client;
    if (out_client) *out_client = NULL;
    if (!transport || !out_client) return MAELYS_HTTP_ERR_ARGUMENT;
    if (!limits) {
        maelys_http_client_limits_default(&defaults);
        limits = &defaults;
    }
    if (limits->io_buffer_bytes < 1024u || limits->io_buffer_bytes > 1024u * 1024u ||
        !limits->max_redirects || !limits->max_informational_responses ||
        limits->max_informational_responses > 1024u ||
        !limits->max_progress_steps_per_advance ||
        limits->max_progress_steps_per_advance > 65536u ||
        !limits->max_wait_slice_ms || limits->max_wait_slice_ms > 60000u ||
        !limits->max_request_body_bytes ||
        !limits->max_connection_reuses ||
        limits->max_connection_reuses > 65536u ||
        !limits->idle_connection_ttl_ms ||
        limits->idle_connection_ttl_ms > 3600000u) {
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    client = calloc(1u, sizeof(*client));
    if (!client) return MAELYS_HTTP_ERR_MEMORY;
    client->transport = transport;
    client->limits = *limits;
    maelys_http_transport_retain(transport);
    *out_client = client;
    return MAELYS_HTTP_OK;
}

static void discard_idle_connection(maelys_http_client_t *client) {
    if (!client->idle_stream) return;
    client->transport->ops.close(client->transport->context,
                                 client->idle_stream);
    client->transport->ops.stream_release(client->transport->context,
                                          client->idle_stream);
    client->idle_stream = NULL;
    free(client->idle_key);
    client->idle_key = NULL;
    client->idle_reuse_count = 0u;
    client->idle_expiry_ms = 0u;
}

maelys_http_result_t maelys_http_client_set_connection_reuse(
    maelys_http_client_t *client, int enabled) {
    if (!client || (enabled != 0 && enabled != 1)) return MAELYS_HTTP_ERR_ARGUMENT;
    client->reuse_enabled = enabled;
    if (!enabled) discard_idle_connection(client);
    return MAELYS_HTTP_OK;
}

void maelys_http_client_release(maelys_http_client_t *client) {
    if (!client) return;
    discard_idle_connection(client);
    maelys_http_transport_release(client->transport);
    free(client);
}

/* Builds "scheme://host:port" with the host lowercased and the port made
 * explicit and numeric, so "HTTPS://Example.COM" and "https://example.com:443"
 * park interchangeably. scheme and authority were already validated. */
static char *make_reuse_key(const char *scheme, const char *authority) {
    size_t scheme_length = strlen(scheme);
    size_t authority_length = strlen(authority);
    size_t host_length = authority_length;
    size_t port_start = 0u;
    unsigned port = 0u;
    size_t index;
    char *key;
    int count;
    if (authority[0] == '[') {
        const char *closing = memchr(authority, ']', authority_length);
        size_t closing_index = (size_t)(closing - authority);
        if (closing_index + 1u < authority_length &&
            authority[closing_index + 1u] == ':') {
            host_length = closing_index + 1u;
            port_start = closing_index + 2u;
        }
    } else {
        const char *colon = memchr(authority, ':', authority_length);
        if (colon) {
            host_length = (size_t)(colon - authority);
            port_start = host_length + 1u;
        }
    }
    if (port_start) {
        for (index = port_start; index < authority_length; ++index) {
            port = port * 10u + (unsigned)(authority[index] - '0');
        }
    } else {
        port = maelys_http_internal_ascii_equal(scheme, scheme_length,
                                                "http") ? 80u : 443u;
    }
    key = malloc(scheme_length + 3u + host_length + 7u);
    if (!key) return NULL;
    memcpy(key, scheme, scheme_length);
    memcpy(key + scheme_length, "://", 3u);
    for (index = 0u; index < host_length; ++index) {
        key[scheme_length + 3u + index] =
            (char)maelys_http_internal_ascii_lower((unsigned char)authority[index]);
    }
    count = snprintf(key + scheme_length + 3u + host_length, 7u, ":%u", port);
    if (count < 2 || count >= 7) {
        free(key);
        return NULL;
    }
    return key;
}

maelys_http_result_t maelys_http_request_config_create(
    const char *method, const char *scheme, const char *authority,
    const char *target, maelys_http_request_t **out_request) {
    maelys_http_request_t *request;
    size_t index;
    if (out_request) *out_request = NULL;
    if (!out_request || !method || !method[0] || !strcmp(method, "CONNECT") ||
        !valid_scheme(scheme) ||
        !valid_authority(authority) ||
        !maelys_http_internal_origin_target_valid(target, strlen(target))) {
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    for (index = 0; method[index]; ++index) {
        if (!maelys_http_internal_is_tchar((unsigned char)method[index])) return MAELYS_HTTP_ERR_ARGUMENT;
    }
    request = calloc(1u, sizeof(*request));
    if (!request) return MAELYS_HTTP_ERR_MEMORY;
    request->method = maelys_http_internal_strdup(method);
    request->scheme = maelys_http_internal_strdup(
        maelys_http_internal_ascii_equal(scheme, strlen(scheme), "http") ?
            "http" : "https");
    request->authority = maelys_http_internal_strdup(authority);
    request->target = maelys_http_internal_strdup(target);
    if (!request->method || !request->scheme || !request->authority || !request->target) {
        maelys_http_request_release(request);
        return MAELYS_HTTP_ERR_MEMORY;
    }
    *out_request = request;
    return MAELYS_HTTP_OK;
}

maelys_http_result_t maelys_http_request_add_header(
    maelys_http_request_t *request, const char *name, const char *value) {
    owned_header_t *headers;
    size_t index;
    if (!request || !name || !name[0] || !value ||
        !maelys_http_internal_header_value_valid(value, strlen(value))) return MAELYS_HTTP_ERR_ARGUMENT;
    for (index = 0; name[index]; ++index) {
        if (!maelys_http_internal_is_tchar((unsigned char)name[index])) return MAELYS_HTTP_ERR_ARGUMENT;
    }
    if (maelys_http_internal_ascii_equal(name, strlen(name), "host") ||
        maelys_http_internal_ascii_equal(name, strlen(name), "content-length") ||
        maelys_http_internal_ascii_equal(name, strlen(name), "transfer-encoding") ||
        maelys_http_internal_ascii_equal(name, strlen(name), "connection") ||
        maelys_http_internal_ascii_equal(name, strlen(name), "upgrade")) {
        return MAELYS_HTTP_ERR_FRAMING;
    }
    if (request->header_count >= 256u) return MAELYS_HTTP_ERR_LIMIT;
    headers = realloc(request->headers,
                      (request->header_count + 1u) * sizeof(*headers));
    if (!headers) return MAELYS_HTTP_ERR_MEMORY;
    request->headers = headers;
    headers[request->header_count].name = maelys_http_internal_strdup(name);
    headers[request->header_count].value = maelys_http_internal_strdup(value);
    if (!headers[request->header_count].name ||
        !headers[request->header_count].value) {
        free(headers[request->header_count].name);
        free(headers[request->header_count].value);
        headers[request->header_count].name = NULL;
        headers[request->header_count].value = NULL;
        return MAELYS_HTTP_ERR_MEMORY;
    }
    ++request->header_count;
    return MAELYS_HTTP_OK;
}

maelys_http_result_t maelys_http_request_set_fixed_body(
    maelys_http_request_t *request, uint64_t content_length,
    maelys_http_body_source_fn source, void *source_context) {
    if (!request || request->body_mode || (!source && content_length)) {
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    request->body_mode = 1;
    request->content_length = content_length;
    request->source = source;
    request->source_context = source_context;
    return MAELYS_HTTP_OK;
}

maelys_http_result_t maelys_http_request_set_chunked_body(
    maelys_http_request_t *request, maelys_http_body_source_fn source,
    void *source_context) {
    if (!request || request->body_mode || !source) return MAELYS_HTTP_ERR_ARGUMENT;
    request->body_mode = 2;
    request->source = source;
    request->source_context = source_context;
    return MAELYS_HTTP_OK;
}

maelys_http_result_t maelys_http_request_set_response_sink(
    maelys_http_request_t *request, maelys_http_body_sink_fn sink,
    void *sink_context) {
    if (!request || !sink) return MAELYS_HTTP_ERR_ARGUMENT;
    request->sink = sink;
    request->sink_context = sink_context;
    return MAELYS_HTTP_OK;
}

maelys_http_result_t maelys_http_request_set_response_headers(
    maelys_http_request_t *request, maelys_http_response_headers_fn headers,
    void *headers_context) {
    if (!request || !headers) return MAELYS_HTTP_ERR_ARGUMENT;
    request->response_headers = headers;
    request->response_headers_context = headers_context;
    return MAELYS_HTTP_OK;
}

maelys_http_result_t maelys_http_request_set_redirect_policy(
    maelys_http_request_t *request, maelys_http_redirect_fn redirect,
    void *redirect_context) {
    if (!request || !redirect) return MAELYS_HTTP_ERR_ARGUMENT;
    request->redirect = redirect;
    request->redirect_context = redirect_context;
    return MAELYS_HTTP_OK;
}

void maelys_http_request_release(maelys_http_request_t *request) {
    if (!request) return;
    free(request->method);
    free(request->scheme);
    free(request->authority);
    free(request->target);
    maelys_http_internal_free_headers(request->headers, request->header_count);
    free(request);
}

static maelys_http_result_t copy_request(
    maelys_http_request_t *destination, const maelys_http_request_t *source) {
    size_t index;
    memset(destination, 0, sizeof(*destination));
    destination->method = maelys_http_internal_strdup(source->method);
    destination->scheme = maelys_http_internal_strdup(source->scheme);
    destination->authority = maelys_http_internal_strdup(source->authority);
    destination->target = maelys_http_internal_strdup(source->target);
    if (!destination->method || !destination->scheme || !destination->authority ||
        !destination->target) return MAELYS_HTTP_ERR_MEMORY;
    destination->body_mode = source->body_mode;
    destination->content_length = source->content_length;
    destination->source = source->source;
    destination->source_context = source->source_context;
    destination->sink = source->sink;
    destination->sink_context = source->sink_context;
    destination->response_headers = source->response_headers;
    destination->response_headers_context = source->response_headers_context;
    destination->redirect = source->redirect;
    destination->redirect_context = source->redirect_context;
    for (index = 0; index < source->header_count; ++index) {
        maelys_http_result_t result = maelys_http_request_add_header(
            destination, source->headers[index].name, source->headers[index].value);
        if (result != MAELYS_HTTP_OK) return result;
    }
    return MAELYS_HTTP_OK;
}

static void clear_request_fields(maelys_http_request_t *request) {
    free(request->method);
    free(request->scheme);
    free(request->authority);
    free(request->target);
    maelys_http_internal_free_headers(request->headers, request->header_count);
    memset(request, 0, sizeof(*request));
}

static maelys_http_result_t checked_add_size(
    size_t *value, size_t amount) {
    if (!value || *value > SIZE_MAX - amount) return MAELYS_HTTP_ERR_LIMIT;
    *value += amount;
    return MAELYS_HTTP_OK;
}

static maelys_http_result_t account_request_header(
    const maelys_http_limits_t *limits, const char *name, const char *value,
    size_t *block_bytes) {
    size_t line_bytes = strlen(name);
    maelys_http_result_t result;
    result = checked_add_size(&line_bytes, 2u);
    if (result == MAELYS_HTTP_OK) {
        result = checked_add_size(&line_bytes, strlen(value));
    }
    if (result == MAELYS_HTTP_OK) result = checked_add_size(&line_bytes, 2u);
    if (result != MAELYS_HTTP_OK || line_bytes > limits->max_header_line_bytes) {
        return MAELYS_HTTP_ERR_LIMIT;
    }
    result = checked_add_size(block_bytes, line_bytes);
    if (result != MAELYS_HTTP_OK || *block_bytes > limits->max_header_bytes) {
        return MAELYS_HTTP_ERR_LIMIT;
    }
    return MAELYS_HTTP_OK;
}

/* Apply the parser's exact wire limits symmetrically to the outgoing head.
 * This runs before any transport open or unbounded serialization buffer.
 * reuse_mode omits the Connection: close line from the head and the budget. */
static maelys_http_result_t validate_request_head(
    const maelys_http_request_t *request,
    const maelys_http_limits_t *limits,
    int reuse_mode) {
    char content_length[32];
    size_t start_line_bytes = strlen(request->method);
    size_t header_count = request->header_count;
    size_t header_bytes = 2u; /* terminating empty line */
    size_t index;
    maelys_http_result_t result;
    int count;
    result = checked_add_size(&start_line_bytes, 1u);
    if (result == MAELYS_HTTP_OK) {
        result = checked_add_size(&start_line_bytes, strlen(request->target));
    }
    if (result == MAELYS_HTTP_OK) {
        result = checked_add_size(&start_line_bytes, 11u); /* " HTTP/1.1\r\n" */
    }
    if (result != MAELYS_HTTP_OK ||
        start_line_bytes > limits->max_start_line_bytes) {
        return MAELYS_HTTP_ERR_LIMIT;
    }
    result = checked_add_size(&header_count,
                              reuse_mode ? 1u : 2u); /* Host (+ Connection) */
    if (result == MAELYS_HTTP_OK && request->body_mode) {
        result = checked_add_size(&header_count, 1u);
    }
    if (result != MAELYS_HTTP_OK || header_count > limits->max_header_count) {
        return MAELYS_HTTP_ERR_LIMIT;
    }
    result = account_request_header(
        limits, "Host", request->authority, &header_bytes);
    if (result == MAELYS_HTTP_OK && !reuse_mode) {
        result = account_request_header(
            limits, "Connection", "close", &header_bytes);
    }
    for (index = 0u; result == MAELYS_HTTP_OK &&
                         index < request->header_count; ++index) {
        result = account_request_header(
            limits, request->headers[index].name,
            request->headers[index].value, &header_bytes);
    }
    if (result == MAELYS_HTTP_OK && request->body_mode == 1) {
        count = snprintf(content_length, sizeof(content_length), "%llu",
                         (unsigned long long)request->content_length);
        if (count < 0 || (size_t)count >= sizeof(content_length)) {
            return MAELYS_HTTP_ERR_LIMIT;
        }
        result = account_request_header(
            limits, "Content-Length", content_length, &header_bytes);
    } else if (result == MAELYS_HTTP_OK && request->body_mode == 2) {
        result = account_request_header(
            limits, "Transfer-Encoding", "chunked", &header_bytes);
    }
    return result;
}

static maelys_http_result_t buffer_sink(
    void *context, const void *bytes, size_t length, size_t *out_written) {
    byte_buffer_t *buffer = context;
    unsigned char *resized;
    if (out_written) *out_written = 0u;
    if (!buffer || (!bytes && length) || !out_written) return MAELYS_HTTP_ERR_ARGUMENT;
    if (buffer->length > SIZE_MAX - length) return MAELYS_HTTP_ERR_LIMIT;
    if (buffer->length + length > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 1024u;
        while (capacity < buffer->length + length) {
            if (capacity > SIZE_MAX / 2u) return MAELYS_HTTP_ERR_LIMIT;
            capacity *= 2u;
        }
        resized = realloc(buffer->bytes, capacity);
        if (!resized) return MAELYS_HTTP_ERR_MEMORY;
        buffer->bytes = resized;
        buffer->capacity = capacity;
    }
    if (length) memcpy(buffer->bytes + buffer->length, bytes, length);
    buffer->length += length;
    *out_written = length;
    return MAELYS_HTTP_OK;
}

static maelys_http_result_t prepare_head(maelys_http_exchange_t *exchange) {
    maelys_http_message_t *message = NULL;
    maelys_http_result_t result;
    size_t index;
    exchange->outgoing.length = 0u;
    exchange->outgoing.offset = 0u;
    result = maelys_http_request_create(exchange->request.method,
                                        exchange->request.target, &message);
    if (result == MAELYS_HTTP_OK) {
        result = maelys_http_message_add_header(message, "Host",
                                                exchange->request.authority);
    }
    if (result == MAELYS_HTTP_OK && !exchange->reuse_mode) {
        result = maelys_http_message_add_header(message, "Connection", "close");
    }
    for (index = 0; result == MAELYS_HTTP_OK &&
                    index < exchange->request.header_count; ++index) {
        result = maelys_http_message_add_header(
            message, exchange->request.headers[index].name,
            exchange->request.headers[index].value);
    }
    if (result == MAELYS_HTTP_OK && exchange->request.body_mode == 1) {
        result = maelys_http_message_set_content_length(
            message, exchange->request.content_length);
    } else if (result == MAELYS_HTTP_OK && exchange->request.body_mode == 2) {
        result = maelys_http_message_set_chunked(message);
    }
    if (result == MAELYS_HTTP_OK) result = maelys_http_message_write_head(
        message, buffer_sink, &exchange->outgoing);
    maelys_http_message_release(message);
    return result;
}

static maelys_http_result_t response_body(
    void *context, const unsigned char *bytes, size_t length) {
    maelys_http_exchange_t *exchange = context;
    maelys_http_sink_step_t step;
    if (!exchange->headers_accepted) {
        exchange->headers_pending = 1;
        return MAELYS_HTTP_AGAIN;
    }
    if (!exchange->request.sink) return MAELYS_HTTP_OK;
    step = exchange->request.sink(exchange->request.sink_context, bytes, length);
    if (step == MAELYS_HTTP_SINK_ACCEPT) {
        return MAELYS_HTTP_OK;
    }
    if (step == MAELYS_HTTP_SINK_PAUSE) {
        exchange->sink_paused = 1;
        return MAELYS_HTTP_AGAIN;
    }
    return MAELYS_HTTP_ERR_IO;
}

/* Return 1 when any response Connection header lists the close token. */
static int connection_names_close(const maelys_http_parser_t *parser) {
    size_t index;
    for (index = 0u; index < maelys_http_parser_header_count(parser); ++index) {
        maelys_http_header_view_t header =
            maelys_http_parser_header(parser, index);
        const char *value;
        size_t remaining;
        if (!maelys_http_internal_ascii_equal(header.name.data,
                                              header.name.length,
                                              "connection")) continue;
        value = header.value.data;
        remaining = header.value.length;
        while (remaining) {
            size_t token_length = 0u;
            if (*value == ' ' || *value == '\t' || *value == ',') {
                ++value;
                --remaining;
                continue;
            }
            while (token_length < remaining && value[token_length] != ',' &&
                   value[token_length] != ' ' &&
                   value[token_length] != '\t') ++token_length;
            if (maelys_http_internal_ascii_equal(value, token_length,
                                                 "close")) return 1;
            value += token_length;
            remaining -= token_length;
        }
    }
    return 0;
}

static int response_is_reusable(const maelys_http_exchange_t *exchange) {
    if (!exchange->reuse_mode || exchange->cancelled) return 0;
    if (maelys_http_parser_result(exchange->parser) != MAELYS_HTTP_COMPLETE) {
        return 0;
    }
    /* An EOF-delimited response consumes the connection by definition. */
    if (maelys_http_parser_body_framing(exchange->parser) ==
        MAELYS_HTTP_BODY_UNTIL_EOF) return 0;
    /* Buffered bytes beyond the framed response are a protocol violation. */
    if (exchange->incoming.offset != exchange->incoming.length) return 0;
    return !connection_names_close(exchange->parser);
}

/* Park the stream as the client's single idle connection, or close it. Only
 * the fully-framed completion path with the request fully sent calls this;
 * every error, timeout, cancellation, redirect, upgrade-refusal and
 * abandonment path closes the stream instead of parking it. */
static void park_or_close_stream(maelys_http_exchange_t *exchange) {
    maelys_http_client_t *client = exchange->client;
    if (client->reuse_enabled && !client->idle_stream &&
        response_is_reusable(exchange)) {
        uint64_t expiry = 0u;
        char *key = make_reuse_key(exchange->request.scheme,
                                   exchange->request.authority);
        if (key && maelys_sys_deadline_after(
                client->limits.idle_connection_ttl_ms,
                &expiry) == MAELYS_SYS_OK) {
            client->idle_stream = exchange->stream;
            client->idle_key = key;
            client->idle_reuse_count = exchange->connection_reuses;
            client->idle_expiry_ms = expiry;
            exchange->stream = NULL;
            return;
        }
        free(key);
    }
    client->transport->ops.close(client->transport->context, exchange->stream);
}

/* Move the parked connection into the exchange when its key, TTL and reuse
 * budget allow; otherwise destroy it so the caller dials fresh. The key
 * covers scheme and canonical authority; the TLS identity travels with the
 * client's single immutable transport. Taking empties the slot, so a second
 * exchange can never share the connection. */
static maelys_http_result_t take_idle_stream(maelys_http_exchange_t *exchange) {
    maelys_http_client_t *client = exchange->client;
    int expired = 0;
    int usable;
    char *key;
    exchange->connection_reuses = 0u;
    if (!exchange->reuse_mode || !client->idle_stream) return MAELYS_HTTP_OK;
    key = make_reuse_key(exchange->request.scheme,
                         exchange->request.authority);
    if (!key) return MAELYS_HTTP_ERR_MEMORY;
    usable = strcmp(key, client->idle_key) == 0 &&
        client->idle_reuse_count < client->limits.max_connection_reuses &&
        maelys_sys_deadline_expired(client->idle_expiry_ms,
                                    &expired) == MAELYS_SYS_OK && !expired;
    if (usable) {
        /* An idle connection must be quiet: a readable byte is a protocol
         * violation and EOF or failure — including a TLS closure without
         * close_notify — is a dead connection. */
        unsigned char probe;
        size_t received = 0u;
        maelys_http_io_step_t step = client->transport->ops.read(
            client->transport->context, client->idle_stream, &probe, 1u,
            &received);
        usable = step == MAELYS_HTTP_IO_WANT_READ ||
                 step == MAELYS_HTTP_IO_WANT_WRITE;
    }
    free(key);
    if (!usable) {
        discard_idle_connection(client);
        return MAELYS_HTTP_OK;
    }
    exchange->stream = client->idle_stream;
    exchange->connection_reuses = client->idle_reuse_count + 1u;
    client->idle_stream = NULL;
    free(client->idle_key);
    client->idle_key = NULL;
    client->idle_reuse_count = 0u;
    client->idle_expiry_ms = 0u;
    return MAELYS_HTTP_OK;
}

maelys_http_result_t maelys_http_exchange_create(
    maelys_http_client_t *client, const maelys_http_request_t *request,
    uint64_t deadline_ms, maelys_http_exchange_t **out_exchange) {
    maelys_http_exchange_t *exchange;
    maelys_http_result_t result;
    if (out_exchange) *out_exchange = NULL;
    if (!client || !request || !out_exchange) return MAELYS_HTTP_ERR_ARGUMENT;
    result = validate_request_head(request, &client->limits.parser,
                                   client->reuse_enabled);
    if (result != MAELYS_HTTP_OK) return result;
    exchange = calloc(1u, sizeof(*exchange));
    if (!exchange) return MAELYS_HTTP_ERR_MEMORY;
    exchange->client = client;
    exchange->reuse_mode = client->reuse_enabled;
    exchange->deadline_ms = deadline_ms;
    result = copy_request(&exchange->request, request);
    if (result == MAELYS_HTTP_OK) {
        exchange->incoming.capacity = client->limits.io_buffer_bytes;
        exchange->incoming.bytes = malloc(exchange->incoming.capacity);
        if (!exchange->incoming.bytes) result = MAELYS_HTTP_ERR_MEMORY;
    }
    if (result == MAELYS_HTTP_OK) result = maelys_http_parser_create(
        MAELYS_HTTP_PARSE_RESPONSE, &client->limits.parser,
        response_body, exchange, &exchange->parser);
    if (result == MAELYS_HTTP_OK && !strcmp(request->method, "HEAD")) {
        result = maelys_http_parser_set_response_to_head(exchange->parser, 1);
    }
    if (result == MAELYS_HTTP_OK && request->body_mode == 1 &&
        request->content_length > client->limits.max_request_body_bytes) {
        result = MAELYS_HTTP_ERR_LIMIT;
    }
    if (result != MAELYS_HTTP_OK) {
        maelys_http_exchange_release(exchange);
        return result;
    }
    exchange->state = EXCHANGE_OPEN;
    *out_exchange = exchange;
    return MAELYS_HTTP_OK;
}

static maelys_http_result_t exchange_fail(
    maelys_http_exchange_t *exchange, maelys_http_result_t result,
    const char *message) {
    if (exchange->state != EXCHANGE_FAILED) {
        exchange->state = EXCHANGE_FAILED;
        maelys_http_internal_set_error(&exchange->error,
                       message ? message : maelys_http_result_string(result));
        if (exchange->stream) {
            exchange->client->transport->ops.close(
                exchange->client->transport->context, exchange->stream);
        }
    }
    return result;
}

static maelys_http_result_t ensure_deadline(maelys_http_exchange_t *exchange) {
    int expired = 0;
    maelys_sys_result_t result = maelys_sys_deadline_expired(
        exchange->deadline_ms, &expired);
    if (result != MAELYS_SYS_OK) return MAELYS_HTTP_ERR_ARGUMENT;
    return expired ? MAELYS_HTTP_ERR_TIMEOUT : MAELYS_HTTP_OK;
}

static maelys_http_result_t take_operation(
    maelys_http_exchange_t *exchange, size_t *remaining) {
    maelys_http_result_t result;
    if (!remaining || !*remaining) return MAELYS_HTTP_AGAIN;
    result = ensure_deadline(exchange);
    if (result != MAELYS_HTTP_OK) return result;
    --*remaining;
    return MAELYS_HTTP_OK;
}

static maelys_http_result_t wait_for(
    maelys_http_exchange_t *exchange, maelys_http_io_step_t step) {
    uint64_t slice_deadline;
    maelys_http_result_t result;
    if (maelys_sys_deadline_after(exchange->client->limits.max_wait_slice_ms,
                                  &slice_deadline) != MAELYS_SYS_OK) {
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    if (exchange->deadline_ms != UINT64_MAX &&
        exchange->deadline_ms < slice_deadline) slice_deadline = exchange->deadline_ms;
    result = exchange->client->transport->ops.wait(
        exchange->client->transport->context, exchange->stream,
        step == MAELYS_HTTP_IO_WANT_READ, step == MAELYS_HTTP_IO_WANT_WRITE,
        slice_deadline);
    if (result == MAELYS_HTTP_AGAIN || result == MAELYS_HTTP_COMPLETE) {
        return MAELYS_HTTP_ERR_STATE;
    }
    if (result == MAELYS_HTTP_ERR_TIMEOUT) {
        result = ensure_deadline(exchange);
        if (result == MAELYS_HTTP_OK) return MAELYS_HTTP_AGAIN;
    }
    return result;
}

static maelys_http_result_t send_pending(
    maelys_http_exchange_t *exchange, size_t *operations) {
    maelys_http_transport_t *transport = exchange->client->transport;
    while (exchange->outgoing.offset < exchange->outgoing.length) {
        size_t written = 0u;
        maelys_http_result_t result = take_operation(exchange, operations);
        if (result != MAELYS_HTTP_OK) return result;
        maelys_http_io_step_t step = transport->ops.write(
            transport->context, exchange->stream,
            exchange->outgoing.bytes + exchange->outgoing.offset,
            exchange->outgoing.length - exchange->outgoing.offset, &written);
        if (step == MAELYS_HTTP_IO_COMPLETE) {
            if (!written || written > exchange->outgoing.length - exchange->outgoing.offset) {
                return MAELYS_HTTP_ERR_IO;
            }
            exchange->outgoing.offset += written;
            continue;
        }
        if (step == MAELYS_HTTP_IO_WANT_READ || step == MAELYS_HTTP_IO_WANT_WRITE) {
            result = wait_for(exchange, step);
            if (result != MAELYS_HTTP_OK) return result;
            continue;
        }
        return MAELYS_HTTP_ERR_IO;
    }
    exchange->outgoing.offset = exchange->outgoing.length = 0u;
    return MAELYS_HTTP_OK;
}

static maelys_http_result_t prepare_body_piece(
    maelys_http_exchange_t *exchange, size_t *operations) {
    unsigned char *body;
    size_t length = 0u;
    maelys_http_source_step_t step;
    size_t capacity = exchange->client->limits.io_buffer_bytes;
    exchange->outgoing.offset = exchange->outgoing.length = 0u;
    if (!exchange->request.body_mode) return MAELYS_HTTP_COMPLETE;
    if (exchange->request.body_mode == 1 &&
        exchange->request_body_sent == exchange->request.content_length) {
        return MAELYS_HTTP_COMPLETE;
    }
    body = malloc(capacity);
    if (!body) return MAELYS_HTTP_ERR_MEMORY;
    {
        maelys_http_result_t result = take_operation(exchange, operations);
        if (result != MAELYS_HTTP_OK) {
            free(body);
            return result;
        }
    }
    step = exchange->request.source ? exchange->request.source(
        exchange->request.source_context, body, capacity, &length) :
        MAELYS_HTTP_SOURCE_END;
    if (step == MAELYS_HTTP_SOURCE_PAUSE) {
        if (length) {
            free(body);
            return MAELYS_HTTP_ERR_STATE;
        }
        free(body);
        return MAELYS_HTTP_AGAIN;
    }
    if ((step != MAELYS_HTTP_SOURCE_DATA &&
         step != MAELYS_HTTP_SOURCE_END) || length > capacity ||
        (step == MAELYS_HTTP_SOURCE_END && length)) {
        free(body);
        return MAELYS_HTTP_ERR_IO;
    }
    if (exchange->request.body_mode == 1) {
        uint64_t remaining = exchange->request.content_length - exchange->request_body_sent;
        if (step == MAELYS_HTTP_SOURCE_END || !length || length > remaining) {
            free(body);
            return MAELYS_HTTP_ERR_FRAMING;
        }
        if (buffer_sink(&exchange->outgoing, body, length, &capacity) != MAELYS_HTTP_OK) {
            free(body);
            return MAELYS_HTTP_ERR_MEMORY;
        }
        exchange->request_body_sent += length;
    } else if (step == MAELYS_HTTP_SOURCE_END) {
        maelys_http_result_t result = maelys_http_write_chunk_end(
            NULL, 0u, buffer_sink, &exchange->outgoing);
        free(body);
        if (result != MAELYS_HTTP_OK) return result;
        exchange->request_body_sent = UINT64_MAX;
        return MAELYS_HTTP_OK;
    } else {
        maelys_http_result_t result;
        if (!length) {
            free(body);
            return MAELYS_HTTP_ERR_IO;
        }
        result = maelys_http_write_chunk(body, length, buffer_sink,
                                         &exchange->outgoing);
        if (result != MAELYS_HTTP_OK) {
            free(body);
            return result;
        }
        if (exchange->request_body_sent > UINT64_MAX - length ||
            exchange->request_body_sent + length >
                exchange->client->limits.max_request_body_bytes) {
            free(body);
            return MAELYS_HTTP_ERR_LIMIT;
        }
        exchange->request_body_sent += length;
    }
    free(body);
    return MAELYS_HTTP_OK;
}

static maelys_http_header_view_t find_header(
    const maelys_http_parser_t *parser, const char *name) {
    size_t index;
    for (index = 0; index < maelys_http_parser_header_count(parser); ++index) {
        maelys_http_header_view_t header = maelys_http_parser_header(parser, index);
        if (maelys_http_internal_ascii_equal(header.name.data, header.name.length, name)) return header;
    }
    {
        maelys_http_header_view_t empty = {{NULL, 0u}, {NULL, 0u}};
        return empty;
    }
}

static int redirect_status(unsigned status) {
    return status == 301u || status == 302u || status == 303u ||
           status == 307u || status == 308u;
}

static maelys_http_result_t parse_location(
    maelys_http_exchange_t *exchange, maelys_http_slice_t location,
    char **out_scheme, char **out_authority, char **out_target) {
    const char *separator;
    const char *path;
    *out_scheme = NULL;
    *out_authority = NULL;
    *out_target = NULL;
    if (!location.data || !location.length || memchr(location.data, '\0', location.length)) {
        return MAELYS_HTTP_ERR_SYNTAX;
    }
    separator = NULL;
    if (location.data[0] != '/' && location.length >= 7u) {
        size_t index;
        for (index = 0; index + 2u < location.length; ++index) {
            if (location.data[index] == ':' && location.data[index + 1u] == '/' &&
                location.data[index + 2u] == '/') {
                separator = location.data + index;
                break;
            }
        }
    }
    if (!separator) {
        if (location.data[0] != '/') return MAELYS_HTTP_ERR_SYNTAX;
        *out_scheme = maelys_http_internal_strdup(exchange->request.scheme);
        if (location.length >= 2u && location.data[1] == '/') {
            const char *authority = location.data + 2u;
            size_t remaining = location.length - 2u;
            size_t authority_length = 0u;
            while (authority_length < remaining &&
                   authority[authority_length] != '/' &&
                   authority[authority_length] != '?') ++authority_length;
            *out_authority = maelys_http_internal_strndup(
                authority, authority_length);
            if (authority_length == remaining) {
                *out_target = maelys_http_internal_strdup("/");
            } else if (authority[authority_length] == '/') {
                *out_target = maelys_http_internal_strndup(
                    authority + authority_length,
                    remaining - authority_length);
            } else {
                *out_target = malloc(remaining - authority_length + 2u);
                if (*out_target) {
                    (*out_target)[0] = '/';
                    memcpy(*out_target + 1u, authority + authority_length,
                           remaining - authority_length);
                    (*out_target)[remaining - authority_length + 1u] = '\0';
                }
            }
        } else {
            *out_authority = maelys_http_internal_strdup(exchange->request.authority);
            *out_target = maelys_http_internal_strndup(location.data, location.length);
        }
    } else {
        size_t scheme_length = (size_t)(separator - location.data);
        const char *authority = separator + 3u;
        size_t remaining = location.length - scheme_length - 3u;
        const char *query = memchr(authority, '?', remaining);
        path = memchr(authority, '/', remaining);
        if (!path || (query && query < path)) path = query;
        *out_scheme = maelys_http_internal_strndup(location.data, scheme_length);
        *out_authority = maelys_http_internal_strndup(authority,
            path ? (size_t)(path - authority) : remaining);
        if (path && *path == '?') {
            size_t target_length = location.length - (size_t)(path - location.data);
            *out_target = malloc(target_length + 2u);
            if (*out_target) {
                (*out_target)[0] = '/';
                memcpy(*out_target + 1u, path, target_length);
                (*out_target)[target_length + 1u] = '\0';
            }
        } else {
            *out_target = path ? maelys_http_internal_strndup(path,
                location.length - (size_t)(path - location.data)) :
                maelys_http_internal_strdup("/");
        }
    }
    if (!*out_scheme || !*out_authority || !*out_target ||
        !valid_scheme(*out_scheme) || !valid_authority(*out_authority) ||
        !maelys_http_internal_origin_target_valid(
            *out_target, *out_target ? strlen(*out_target) : 0u)) {
        free(*out_scheme); free(*out_authority); free(*out_target);
        *out_scheme = *out_authority = *out_target = NULL;
        return MAELYS_HTTP_ERR_SYNTAX;
    }
    {
        size_t index;
        for (index = 0u; (*out_scheme)[index]; ++index) {
            (*out_scheme)[index] = (char)maelys_http_internal_ascii_lower(
                (unsigned char)(*out_scheme)[index]);
        }
    }
    return MAELYS_HTTP_OK;
}

static void strip_sensitive_headers(maelys_http_request_t *request) {
    size_t source;
    size_t destination = 0u;
    for (source = 0; source < request->header_count; ++source) {
        owned_header_t *header = &request->headers[source];
        int sensitive = maelys_http_internal_ascii_equal(header->name, strlen(header->name), "authorization") ||
            maelys_http_internal_ascii_equal(header->name, strlen(header->name), "cookie") ||
            maelys_http_internal_ascii_equal(header->name, strlen(header->name), "proxy-authorization");
        if (sensitive) {
            free(header->name);
            free(header->value);
        } else {
            if (destination != source) request->headers[destination] = *header;
            ++destination;
        }
    }
    request->header_count = destination;
}

static maelys_http_result_t maybe_redirect(maelys_http_exchange_t *exchange) {
    unsigned status = maelys_http_parser_status(exchange->parser);
    maelys_http_header_view_t location;
    char *scheme = NULL;
    char *authority = NULL;
    char *target = NULL;
    char *new_method = NULL;
    int authority_changed;
    maelys_http_redirect_decision_t decision;
    maelys_http_result_t result;
    maelys_http_slice_t old_authority;
    maelys_http_slice_t new_scheme;
    maelys_http_slice_t new_authority;
    maelys_http_slice_t new_target;
    size_t location_count = 0u;
    size_t header_index;
    if (exchange->redirect_checked) return MAELYS_HTTP_OK;
    if (!redirect_status(status) || !exchange->request.redirect) {
        exchange->redirect_checked = 1;
        return MAELYS_HTTP_OK;
    }
    location = find_header(exchange->parser, "location");
    if (!location.value.data) {
        exchange->redirect_checked = 1;
        return MAELYS_HTTP_OK;
    }
    for (header_index = 0u;
         header_index < maelys_http_parser_header_count(exchange->parser);
         ++header_index) {
        maelys_http_header_view_t candidate =
            maelys_http_parser_header(exchange->parser, header_index);
        if (maelys_http_internal_ascii_equal(candidate.name.data,
                                              candidate.name.length,
                                              "location")) ++location_count;
    }
    if (location_count != 1u) return MAELYS_HTTP_ERR_FRAMING;
    result = parse_location(exchange, location.value, &scheme, &authority, &target);
    if (result != MAELYS_HTTP_OK) return result;
    old_authority.data = exchange->request.authority;
    old_authority.length = strlen(exchange->request.authority);
    new_scheme.data = scheme; new_scheme.length = strlen(scheme);
    new_authority.data = authority; new_authority.length = strlen(authority);
    new_target.data = target; new_target.length = strlen(target);
    decision = exchange->request.redirect(exchange->request.redirect_context,
        status, old_authority, new_scheme, new_authority, new_target,
        exchange->redirect_count + 1u);
    exchange->redirect_checked = 1;
    if (decision != MAELYS_HTTP_REDIRECT_FOLLOW) {
        free(scheme); free(authority); free(target);
        return MAELYS_HTTP_OK;
    }
    if (exchange->redirect_count >= exchange->client->limits.max_redirects) {
        free(scheme); free(authority); free(target);
        return MAELYS_HTTP_ERR_LIMIT;
    }
    if (exchange->request.body_mode && status != 303u) {
        free(scheme); free(authority); free(target);
        return MAELYS_HTTP_ERR_STATE;
    }
    if (status == 303u && strcmp(exchange->request.method, "HEAD")) {
        new_method = maelys_http_internal_strdup("GET");
        if (!new_method) {
            free(scheme); free(authority); free(target);
            return MAELYS_HTTP_ERR_MEMORY;
        }
    }
    authority_changed = strcmp(exchange->request.authority, authority) != 0 ||
                        strcmp(exchange->request.scheme, scheme) != 0;
    if (authority_changed) strip_sensitive_headers(&exchange->request);
    free(exchange->request.scheme);
    free(exchange->request.authority);
    free(exchange->request.target);
    exchange->request.scheme = scheme;
    exchange->request.authority = authority;
    exchange->request.target = target;
    if (status == 303u) {
        if (new_method) {
            free(exchange->request.method);
            exchange->request.method = new_method;
        }
        exchange->request.body_mode = 0;
        exchange->request.source = NULL;
        exchange->request.source_context = NULL;
    }
    exchange->client->transport->ops.close(
        exchange->client->transport->context, exchange->stream);
    exchange->client->transport->ops.stream_release(
        exchange->client->transport->context, exchange->stream);
    exchange->stream = NULL;
    exchange->incoming.offset = exchange->incoming.length = 0u;
    exchange->request_body_sent = 0u;
    exchange->headers_pending = 0;
    exchange->headers_accepted = 0;
    exchange->redirect_checked = 0;
    ++exchange->redirect_count;
    (void)maelys_http_parser_reset(exchange->parser);
    if (!strcmp(exchange->request.method, "HEAD")) {
        (void)maelys_http_parser_set_response_to_head(exchange->parser, 1);
    }
    exchange->state = EXCHANGE_OPEN;
    return MAELYS_HTTP_AGAIN;
}

static maelys_http_result_t process_response_headers(
    maelys_http_exchange_t *exchange) {
    maelys_http_result_t result;
    maelys_http_headers_step_t step;
    if (exchange->headers_accepted) {
        exchange->headers_pending = 0;
        return MAELYS_HTTP_OK;
    }
    result = maybe_redirect(exchange);
    if (result != MAELYS_HTTP_OK) return result;
    if (!exchange->request.response_headers) {
        exchange->headers_accepted = 1;
        exchange->headers_pending = 0;
        return MAELYS_HTTP_OK;
    }
    step = exchange->request.response_headers(
        exchange->request.response_headers_context, exchange);
    if (step == MAELYS_HTTP_HEADERS_ACCEPT) {
        exchange->headers_accepted = 1;
        exchange->headers_pending = 0;
        return MAELYS_HTTP_OK;
    }
    if (step == MAELYS_HTTP_HEADERS_PAUSE) return MAELYS_HTTP_AGAIN;
    return MAELYS_HTTP_ERR_IO;
}

static maelys_http_result_t read_response(
    maelys_http_exchange_t *exchange, size_t *operations,
    int wait_when_blocked) {
    maelys_http_transport_t *transport = exchange->client->transport;
    for (;;) {
        maelys_http_result_t result;
        size_t consumed = 0u;
        if (exchange->headers_pending) {
            result = process_response_headers(exchange);
            if (result != MAELYS_HTTP_OK) return result;
            if (maelys_http_parser_result(exchange->parser) ==
                MAELYS_HTTP_COMPLETE) return MAELYS_HTTP_COMPLETE;
        }
        if (exchange->incoming.offset < exchange->incoming.length) {
            result = take_operation(exchange, operations);
            if (result != MAELYS_HTTP_OK) return result;
            exchange->sink_paused = 0;
            result = maelys_http_parser_feed(exchange->parser,
                exchange->incoming.bytes + exchange->incoming.offset,
                exchange->incoming.length - exchange->incoming.offset, &consumed);
            exchange->incoming.offset += consumed;
            if (exchange->sink_paused) return MAELYS_HTTP_AGAIN;
            if (maelys_http_parser_headers_complete(exchange->parser) &&
                maelys_http_parser_status(exchange->parser) / 100u != 1u &&
                !exchange->headers_accepted) {
                exchange->headers_pending = 1;
            }
            if (exchange->headers_pending) {
                maelys_http_result_t parse_result = result;
                result = process_response_headers(exchange);
                if (result != MAELYS_HTTP_OK) return result;
                result = parse_result;
                if (result == MAELYS_HTTP_AGAIN) continue;
            }
            if (result == MAELYS_HTTP_COMPLETE) {
                unsigned status = maelys_http_parser_status(exchange->parser);
                if (status / 100u == 1u) {
                    if (status == 101u) return MAELYS_HTTP_ERR_FRAMING;
                    ++exchange->informational_count;
                    if (exchange->informational_count >
                        exchange->client->limits.max_informational_responses) {
                        return MAELYS_HTTP_ERR_LIMIT;
                    }
                    result = maelys_http_parser_reset(exchange->parser);
                    if (result != MAELYS_HTTP_OK) return result;
                    if (!strcmp(exchange->request.method, "HEAD")) {
                        result = maelys_http_parser_set_response_to_head(
                            exchange->parser, 1);
                        if (result != MAELYS_HTTP_OK) return result;
                    }
                    exchange->headers_pending = 0;
                    exchange->headers_accepted = 0;
                    exchange->redirect_checked = 0;
                    continue;
                }
                exchange->headers_pending = 1;
                result = process_response_headers(exchange);
                return result == MAELYS_HTTP_OK ? MAELYS_HTTP_COMPLETE : result;
            }
            if (result != MAELYS_HTTP_AGAIN) return result;
            if (exchange->incoming.offset < exchange->incoming.length) continue;
        }
        exchange->incoming.offset = exchange->incoming.length = 0u;
        {
            size_t received = 0u;
            result = take_operation(exchange, operations);
            if (result != MAELYS_HTTP_OK) return result;
            maelys_http_io_step_t step = transport->ops.read(
                transport->context, exchange->stream, exchange->incoming.bytes,
                exchange->incoming.capacity, &received);
            if (step == MAELYS_HTTP_IO_COMPLETE) {
                if (!received || received > exchange->incoming.capacity) return MAELYS_HTTP_ERR_IO;
                exchange->incoming.length = received;
                continue;
            }
            if (step == MAELYS_HTTP_IO_CLOSED) {
                result = maelys_http_parser_eof(exchange->parser);
                if (result != MAELYS_HTTP_COMPLETE) return result;
                exchange->headers_pending = 1;
                result = process_response_headers(exchange);
                return result == MAELYS_HTTP_OK ? MAELYS_HTTP_COMPLETE : result;
            }
            if (step == MAELYS_HTTP_IO_WANT_READ || step == MAELYS_HTTP_IO_WANT_WRITE) {
                if (!wait_when_blocked) return MAELYS_HTTP_AGAIN;
                result = wait_for(exchange, step);
                if (result != MAELYS_HTTP_OK) return result;
                continue;
            }
            return MAELYS_HTTP_ERR_IO;
        }
    }
}

maelys_http_result_t maelys_http_exchange_advance(maelys_http_exchange_t *exchange) {
    maelys_http_result_t result;
    size_t operations;
    if (!exchange) return MAELYS_HTTP_ERR_ARGUMENT;
    if (exchange->state == EXCHANGE_DONE) return MAELYS_HTTP_COMPLETE;
    if (exchange->state == EXCHANGE_FAILED) return MAELYS_HTTP_ERR_STATE;
    if (exchange->cancelled) return exchange_fail(
        exchange, MAELYS_HTTP_ERR_CANCELLED, "HTTP exchange cancelled");
    result = ensure_deadline(exchange);
    if (result != MAELYS_HTTP_OK) return exchange_fail(exchange, result, NULL);
    operations = exchange->client->limits.max_progress_steps_per_advance;
    for (;;) {
        if (exchange->state == EXCHANGE_OPEN) {
            char *open_error = NULL;
            result = validate_request_head(
                &exchange->request, &exchange->client->limits.parser,
                exchange->reuse_mode);
            if (result != MAELYS_HTTP_OK) {
                return exchange_fail(exchange, result, NULL);
            }
            result = take_operation(exchange, &operations);
            if (result == MAELYS_HTTP_AGAIN) return result;
            if (result != MAELYS_HTTP_OK) return exchange_fail(exchange, result, NULL);
            result = take_idle_stream(exchange);
            if (result != MAELYS_HTTP_OK) return exchange_fail(exchange, result, NULL);
            if (!exchange->stream) {
                result = exchange->client->transport->ops.open(
                    exchange->client->transport->context, exchange->request.scheme,
                    exchange->request.authority, exchange->deadline_ms,
                    &exchange->stream, &open_error);
                if (result == MAELYS_HTTP_AGAIN || result == MAELYS_HTTP_COMPLETE) {
                    result = MAELYS_HTTP_ERR_STATE;
                }
                if (result != MAELYS_HTTP_OK || !exchange->stream) {
                    result = exchange_fail(exchange,
                        result == MAELYS_HTTP_OK ? MAELYS_HTTP_ERR_IO : result,
                        open_error ? open_error : "transport open failed");
                    free(open_error);
                    return result;
                }
                free(open_error);
            }
            result = prepare_head(exchange);
            if (result != MAELYS_HTTP_OK) return exchange_fail(exchange, result, NULL);
            exchange->state = EXCHANGE_SEND_HEAD;
        }
        if (exchange->state == EXCHANGE_SEND_HEAD) {
            result = send_pending(exchange, &operations);
            if (result == MAELYS_HTTP_AGAIN) return result;
            if (result != MAELYS_HTTP_OK) return exchange_fail(exchange, result, NULL);
            exchange->state = exchange->request.body_mode ?
                EXCHANGE_SEND_BODY : EXCHANGE_READ;
            exchange->upload_probe_due = exchange->request.body_mode ? 1 : 0;
        }
        if (exchange->state == EXCHANGE_SEND_BODY) {
            if (exchange->upload_probe_due) {
                if (!operations) return MAELYS_HTTP_AGAIN;
                result = read_response(exchange, &operations, 0);
                if (result == MAELYS_HTTP_COMPLETE) {
                    /* Early final response: the request body was cut short,
                     * so this connection is closed, never parked. */
                    exchange->state = EXCHANGE_DONE;
                    exchange->client->transport->ops.close(
                        exchange->client->transport->context, exchange->stream);
                    return MAELYS_HTTP_COMPLETE;
                }
                if (result == MAELYS_HTTP_AGAIN) {
                    if (exchange->state == EXCHANGE_OPEN) continue;
                    if (exchange->sink_paused) return result;
                    if (maelys_http_parser_headers_complete(exchange->parser) &&
                        maelys_http_parser_status(exchange->parser) / 100u != 1u) {
                        exchange->state = EXCHANGE_READ;
                        continue;
                    }
                    exchange->upload_probe_due = 0;
                    if (!operations) return result;
                } else {
                    const char *message = result == MAELYS_HTTP_ERR_IO ||
                        result == MAELYS_HTTP_ERR_TLS ?
                        exchange->client->transport->ops.last_error(
                            exchange->client->transport->context, exchange->stream) :
                        maelys_http_result_string(result);
                    return exchange_fail(exchange, result, message);
                }
            }
            if (exchange->outgoing.length) {
                result = send_pending(exchange, &operations);
                if (result == MAELYS_HTTP_AGAIN) {
                    exchange->upload_probe_due = 1;
                    return result;
                }
                if (result != MAELYS_HTTP_OK) return exchange_fail(exchange, result, NULL);
                exchange->upload_probe_due = 1;
                if (exchange->request.body_mode == 2 &&
                    exchange->request_body_sent == UINT64_MAX) {
                    exchange->state = EXCHANGE_READ;
                    continue;
                }
            }
            result = prepare_body_piece(exchange, &operations);
            if (result == MAELYS_HTTP_AGAIN) {
                exchange->upload_probe_due = 1;
                return result;
            }
            if (result == MAELYS_HTTP_COMPLETE) {
                exchange->state = EXCHANGE_READ;
                continue;
            }
            if (result != MAELYS_HTTP_OK) return exchange_fail(exchange, result, NULL);
            continue;
        }
        if (exchange->state == EXCHANGE_READ) {
            result = read_response(exchange, &operations, 1);
            if (result == MAELYS_HTTP_AGAIN) {
                if (exchange->state == EXCHANGE_OPEN) continue;
                return result;
            }
            if (result != MAELYS_HTTP_COMPLETE) {
                const char *message = result == MAELYS_HTTP_ERR_IO ||
                    result == MAELYS_HTTP_ERR_TLS ?
                    exchange->client->transport->ops.last_error(
                        exchange->client->transport->context, exchange->stream) :
                    maelys_http_result_string(result);
                return exchange_fail(exchange, result, message);
            }
            exchange->state = EXCHANGE_DONE;
            park_or_close_stream(exchange);
            return MAELYS_HTTP_COMPLETE;
        }
    }
}

void maelys_http_exchange_cancel(maelys_http_exchange_t *exchange) {
    if (!exchange || exchange->cancelled || exchange->state == EXCHANGE_DONE ||
        exchange->state == EXCHANGE_FAILED) return;
    exchange->cancelled = 1;
    if (exchange->stream) {
        exchange->client->transport->ops.cancel(
            exchange->client->transport->context, exchange->stream);
        exchange->client->transport->ops.close(
            exchange->client->transport->context, exchange->stream);
    }
}

unsigned maelys_http_exchange_status(const maelys_http_exchange_t *exchange) {
    return exchange && exchange->parser ? maelys_http_parser_status(exchange->parser) : 0u;
}
size_t maelys_http_exchange_header_count(const maelys_http_exchange_t *exchange) {
    return exchange && exchange->parser ? maelys_http_parser_header_count(exchange->parser) : 0u;
}
maelys_http_header_view_t maelys_http_exchange_header(
    const maelys_http_exchange_t *exchange, size_t index) {
    if (exchange && exchange->parser) return maelys_http_parser_header(exchange->parser, index);
    {
        maelys_http_header_view_t empty = {{NULL, 0u}, {NULL, 0u}};
        return empty;
    }
}
size_t maelys_http_exchange_trailer_count(const maelys_http_exchange_t *exchange) {
    return exchange && exchange->parser ? maelys_http_parser_trailer_count(exchange->parser) : 0u;
}
maelys_http_header_view_t maelys_http_exchange_trailer(
    const maelys_http_exchange_t *exchange, size_t index) {
    if (exchange && exchange->parser) return maelys_http_parser_trailer(exchange->parser, index);
    {
        maelys_http_header_view_t empty = {{NULL, 0u}, {NULL, 0u}};
        return empty;
    }
}
const char *maelys_http_exchange_error(const maelys_http_exchange_t *exchange) {
    return exchange ? exchange->error : NULL;
}

void maelys_http_exchange_release(maelys_http_exchange_t *exchange) {
    if (!exchange) return;
    if (exchange->stream) {
        exchange->client->transport->ops.close(
            exchange->client->transport->context, exchange->stream);
        exchange->client->transport->ops.stream_release(
            exchange->client->transport->context, exchange->stream);
    }
    maelys_http_parser_release(exchange->parser);
    clear_request_fields(&exchange->request);
    free(exchange->outgoing.bytes);
    free(exchange->incoming.bytes);
    free(exchange->error);
    free(exchange);
}
