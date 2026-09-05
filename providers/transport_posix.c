#include "maelys/http_transports.h"

#include "resolver_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "maelys/sys/fd.h"
#include "maelys/sys/socket.h"
#include "maelys/sys/version.h"

#if MAELYS_SYS_ABI_VERSION != 1u
#error "maelys-http targets Maelys System ABI 1"
#endif

typedef struct posix_context {
    maelys_http_tls_provider_t *tls_provider;
    maelys_http_resolver_t *resolver;
} posix_context_t;

typedef struct posix_stream {
    int resolution_consumed;
    maelys_http_resolver_request_t *resolver_request;
    maelys_http_resolver_address_t addresses[MAELYS_HTTP_RESOLVER_MAX_ADDRESSES];
    size_t address_count;
    size_t current_address;
    size_t address_attempts;
    int connect_in_progress;
    char *host;
    uint16_t port;
    char *scheme;
    maelys_http_tls_provider_t *tls_provider;
    maelys_sys_socket_t *socket_handle;
    int closed;
    int cancelled;
    int ready;
    int open_failed;
    maelys_http_tls_session_t *tls;
    char error[256];
} posix_stream_t;

static void remember(posix_stream_t *stream, const char *message) {
    if (!stream) return;
    (void)snprintf(stream->error, sizeof(stream->error), "%s",
                   message ? message : "POSIX transport failure");
}

static maelys_http_result_t split_authority(
    const char *scheme, const char *authority,
    char **out_host, uint16_t *out_port) {
    const char *host_start = authority;
    const char *host_end;
    const char *service = !strcmp(scheme, "https") ? "443" : "80";
    size_t host_length;
    unsigned long port;
    char *end = NULL;
    if (out_host) *out_host = NULL;
    if (out_port) *out_port = 0u;
    if (!scheme || !authority || !authority[0] || !out_host || !out_port ||
        strchr(authority, '@')) return MAELYS_HTTP_ERR_ARGUMENT;
    if (authority[0] == '[') {
        host_start = authority + 1u;
        host_end = strchr(host_start, ']');
        if (!host_end || host_end == host_start ||
            (host_end[1] && host_end[1] != ':')) return MAELYS_HTTP_ERR_ARGUMENT;
        if (host_end[1] == ':') {
            if (!host_end[2]) return MAELYS_HTTP_ERR_ARGUMENT;
            service = host_end + 2u;
        }
    } else {
        const char *colon = strrchr(authority, ':');
        if (colon && strchr(authority, ':') != colon) {
            return MAELYS_HTTP_ERR_ARGUMENT;
        }
        if (colon && strchr(authority, ':') == colon) {
            if (colon == authority || !colon[1]) return MAELYS_HTTP_ERR_ARGUMENT;
            host_end = colon;
            service = colon + 1u;
        } else {
            host_end = authority + strlen(authority);
        }
    }
    host_length = (size_t)(host_end - host_start);
    *out_host = malloc(host_length + 1u);
    if (!*out_host) return MAELYS_HTTP_ERR_MEMORY;
    memcpy(*out_host, host_start, host_length);
    (*out_host)[host_length] = '\0';
    errno = 0;
    port = strtoul(service, &end, 10);
    if (errno || !end || *end || port == 0ul || port > 65535ul) {
        free(*out_host);
        *out_host = NULL;
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    *out_port = (uint16_t)port;
    return MAELYS_HTTP_OK;
}

static void destroy_stream(posix_stream_t *stream) {
    if (!stream) return;
    if (stream->resolver_request) {
        (void)maelys_http_resolver_cancel_internal(stream->resolver_request);
        maelys_http_resolver_request_release_internal(stream->resolver_request);
    }
    (void)maelys_sys_socket_release(&stream->socket_handle);
    maelys_http_tls_session_release(stream->tls);
    maelys_http_tls_provider_release(stream->tls_provider);
    free(stream->host);
    free(stream->scheme);
    free(stream);
}

/* One readiness wait on the connection's descriptor. A stream owns exactly
 * one descriptor and needs no reactor: System's fd_wait is a single poll(2)
 * bounded by the absolute deadline. ERROR and HUP are only indications and
 * the hosts report them differently (Linux adds POLLERR after a reset,
 * macOS does not), so readiness is reported as such and the receive, send,
 * connect_complete or handshake that follows carries the precise cause. */
static maelys_http_result_t stream_wait(
    posix_stream_t *stream, int want_read, int want_write,
    uint64_t deadline_ms) {
    unsigned interests = (want_read ? MAELYS_SYS_INTEREST_READ : 0u) |
                         (want_write ? MAELYS_SYS_INTEREST_WRITE : 0u);
    unsigned flags = 0u;
    maelys_sys_result_t result;
    if (!stream || stream->closed || !interests || !stream->socket_handle) {
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    result = maelys_sys_fd_wait(
        maelys_sys_socket_native_fd(stream->socket_handle), interests,
        deadline_ms, &flags);
    if (result == MAELYS_SYS_ERR_TIMEOUT) return MAELYS_HTTP_ERR_TIMEOUT;
    if (result != MAELYS_SYS_OK) {
        remember(stream, "readiness wait failed");
        return MAELYS_HTTP_ERR_IO;
    }
    return MAELYS_HTTP_OK;
}

static void discard_connection(posix_stream_t *stream) {
    if (!stream) return;
    (void)maelys_sys_socket_release(&stream->socket_handle);
}

static maelys_http_result_t open_stream(
    void *opaque, const char *scheme, const char *authority,
    uint64_t deadline_ms, void **out_stream, char **out_error) {
    posix_context_t *context = opaque;
    posix_stream_t *stream;
    maelys_http_result_t result;
    if (out_stream) *out_stream = NULL;
    if (out_error) *out_error = NULL;
    if (!context || !out_stream || !scheme ||
        (strcmp(scheme, "http") && strcmp(scheme, "https"))) {
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    if (!strcmp(scheme, "https") && !context->tls_provider) {
        if (out_error) *out_error = strdup("HTTPS requires a TLS provider");
        return MAELYS_HTTP_ERR_TLS;
    }
    stream = calloc(1u, sizeof(*stream));
    if (!stream) return MAELYS_HTTP_ERR_MEMORY;
    result = split_authority(scheme, authority, &stream->host, &stream->port);
    if (result != MAELYS_HTTP_OK) {
        destroy_stream(stream);
        return result;
    }
    stream->scheme = strdup(scheme);
    (void)deadline_ms;
    stream->tls_provider = context->tls_provider;
    maelys_http_tls_provider_retain(stream->tls_provider);
    if (!stream->scheme) {
        destroy_stream(stream);
        return MAELYS_HTTP_ERR_MEMORY;
    }
    result = maelys_http_resolver_start_internal(
        context->resolver, stream->host, stream->port, deadline_ms,
        &stream->resolver_request, out_error);
    if (result != MAELYS_HTTP_OK) {
        destroy_stream(stream);
        return result;
    }
    *out_stream = stream;
    return MAELYS_HTTP_OK;
}

static maelys_http_result_t wait_for_resolution(
    posix_stream_t *stream, uint64_t deadline_ms) {
    unsigned flags = 0u;
    maelys_sys_result_t wait_result;
    maelys_http_result_t take_result;
    char *resolver_error = NULL;
    int notification_fd;
    if (!stream) return MAELYS_HTTP_ERR_ARGUMENT;
    if (stream->resolution_consumed) return MAELYS_HTTP_OK;
    if (!stream->resolver_request) return MAELYS_HTTP_ERR_STATE;
    notification_fd = maelys_http_resolver_notification_fd_internal(
        stream->resolver_request);
    if (notification_fd < 0) {
        remember(stream, "resolver notification descriptor is invalid");
        return MAELYS_HTTP_ERR_IO;
    }
    wait_result = maelys_sys_fd_wait(notification_fd, MAELYS_SYS_INTEREST_READ,
                                     deadline_ms, &flags);
    if (wait_result == MAELYS_SYS_ERR_TIMEOUT) return MAELYS_HTTP_ERR_TIMEOUT;
    if (wait_result != MAELYS_SYS_OK) {
        remember(stream, "DNS readiness wait failed");
        return MAELYS_HTTP_ERR_IO;
    }
    take_result = maelys_http_resolver_take_internal(
        stream->resolver_request, stream->addresses,
        MAELYS_HTTP_RESOLVER_MAX_ADDRESSES, &stream->address_count,
        &resolver_error);
    if (take_result == MAELYS_HTTP_AGAIN) {
        remember(stream, flags & (MAELYS_SYS_EVENT_ERROR | MAELYS_SYS_EVENT_HUP) ?
            "resolver worker or notification channel died" :
            "resolver signalled readiness without a terminal response");
        return MAELYS_HTTP_ERR_IO;
    }
    if (take_result != MAELYS_HTTP_OK) {
        remember(stream, resolver_error ? resolver_error : "DNS resolution failed");
        free(resolver_error);
        return take_result;
    }
    free(resolver_error);
    maelys_http_resolver_request_release_internal(stream->resolver_request);
    stream->resolver_request = NULL;
    stream->resolution_consumed = 1;
    return MAELYS_HTTP_OK;
}

static maelys_http_result_t begin_next_connection(posix_stream_t *stream) {
    while (stream->current_address < stream->address_count &&
           stream->address_attempts < MAELYS_HTTP_RESOLVER_MAX_ADDRESSES) {
        const maelys_http_resolver_address_t *address =
            &stream->addresses[stream->current_address++];
        struct sockaddr_storage storage;
        const struct sockaddr *native_address;
        socklen_t native_length;
        int family;
        maelys_sys_connect_state_t state;
        maelys_sys_result_t connect_result;
        memset(&storage, 0, sizeof(storage));
        if (address->family == MAELYS_HTTP_RESOLVER_IPV4) {
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)&storage;
            ipv4->sin_family = AF_INET;
            ipv4->sin_port = htons(address->port);
            memcpy(&ipv4->sin_addr, address->bytes, 4u);
            native_length = (socklen_t)sizeof(*ipv4);
            family = AF_INET;
        } else {
            struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)&storage;
            ipv6->sin6_family = AF_INET6;
            ipv6->sin6_port = htons(address->port);
            memcpy(&ipv6->sin6_addr, address->bytes, 16u);
            ipv6->sin6_scope_id = address->scope_id;
            native_length = (socklen_t)sizeof(*ipv6);
            family = AF_INET6;
        }
        native_address = (const struct sockaddr *)&storage;
        ++stream->address_attempts;
        if (maelys_sys_socket_create(
                family, SOCK_STREAM, IPPROTO_TCP,
                &stream->socket_handle) != MAELYS_SYS_OK) {
            discard_connection(stream);
            continue;
        }
        connect_result = maelys_sys_socket_connect_start(
            stream->socket_handle, native_address, native_length, &state);
        if (connect_result == MAELYS_SYS_OK) {
            stream->connect_in_progress =
                state == MAELYS_SYS_CONNECT_IN_PROGRESS;
            return MAELYS_HTTP_OK;
        }
        discard_connection(stream);
    }
    remember(stream,
             stream->address_attempts >= MAELYS_HTTP_RESOLVER_MAX_ADDRESSES ?
             "DNS address result limit exceeded" : "connection failed");
    stream->open_failed = 1;
    return stream->address_attempts >= MAELYS_HTTP_RESOLVER_MAX_ADDRESSES ?
        MAELYS_HTTP_ERR_LIMIT : MAELYS_HTTP_ERR_IO;
}

static maelys_http_result_t progress_open(
    posix_stream_t *stream, uint64_t deadline_ms) {
    maelys_http_result_t result;
    if (!stream || stream->closed || stream->cancelled) {
        return MAELYS_HTTP_ERR_CANCELLED;
    }
    if (stream->ready) return MAELYS_HTTP_OK;
    if (stream->open_failed) return MAELYS_HTTP_ERR_IO;
    result = wait_for_resolution(stream, deadline_ms);
    if (result != MAELYS_HTTP_OK) return result;
    if (!stream->socket_handle) {
        result = begin_next_connection(stream);
        if (result != MAELYS_HTTP_OK) return result;
    }
    if (stream->connect_in_progress) {
        result = stream_wait(stream, 0, 1, deadline_ms);
        if (result != MAELYS_HTTP_OK) return result;
        if (maelys_sys_socket_connect_complete(stream->socket_handle) !=
            MAELYS_SYS_OK) {
            discard_connection(stream);
            return begin_next_connection(stream);
        }
        stream->connect_in_progress = 0;
        return MAELYS_HTTP_OK;
    }
    if (!strcmp(stream->scheme, "https")) {
        maelys_http_io_step_t step;
        if (!stream->tls) {
            char *tls_error = NULL;
            result = maelys_http_tls_session_create_client(
                stream->tls_provider,
                maelys_sys_socket_native_fd(stream->socket_handle), stream->host,
                &stream->tls, &tls_error);
            if (result != MAELYS_HTTP_OK) {
                remember(stream, tls_error ? tls_error :
                         "TLS session creation failed");
                free(tls_error);
                stream->open_failed = 1;
                return result;
            }
            free(tls_error);
        }
        step = maelys_http_tls_session_handshake(stream->tls);
        if (step == MAELYS_HTTP_IO_COMPLETE) {
            stream->ready = 1;
            return MAELYS_HTTP_OK;
        }
        if (step != MAELYS_HTTP_IO_WANT_READ &&
            step != MAELYS_HTTP_IO_WANT_WRITE) {
            remember(stream, maelys_http_tls_session_error(stream->tls));
            stream->open_failed = 1;
            return MAELYS_HTTP_ERR_TLS;
        }
        return stream_wait(stream, step == MAELYS_HTTP_IO_WANT_READ,
                           step == MAELYS_HTTP_IO_WANT_WRITE, deadline_ms);
    }
    stream->ready = 1;
    return MAELYS_HTTP_OK;
}

static maelys_http_io_step_t raw_read(
    void *context, void *opaque, void *buffer, size_t capacity,
    size_t *out_read) {
    posix_stream_t *stream = opaque;
    maelys_sys_result_t result;
    (void)context;
    if (out_read) *out_read = 0u;
    if (!stream || stream->closed || !buffer || !capacity || !out_read) {
        return MAELYS_HTTP_IO_FAILED;
    }
    if (!stream->ready) {
        return stream->open_failed ? MAELYS_HTTP_IO_FAILED : MAELYS_HTTP_IO_WANT_READ;
    }
    if (stream->tls) return maelys_http_tls_session_read(
        stream->tls, buffer, capacity, out_read);
    result = maelys_sys_socket_receive(
        stream->socket_handle, buffer, capacity, out_read);
    if (result == MAELYS_SYS_OK) return MAELYS_HTTP_IO_COMPLETE;
    if (result == MAELYS_SYS_ERR_WOULD_BLOCK) return MAELYS_HTTP_IO_WANT_READ;
    /* Only a clean end of stream is CLOSED. A peer reset is a failure: a
     * body delimited by the close must never pass as complete after it. */
    if (result == MAELYS_SYS_ERR_CLOSED) return MAELYS_HTTP_IO_CLOSED;
    if (result == MAELYS_SYS_ERR_RESET) {
        remember(stream, "connection reset by peer");
        return MAELYS_HTTP_IO_FAILED;
    }
    remember(stream, strerror(errno));
    return MAELYS_HTTP_IO_FAILED;
}

static maelys_http_io_step_t raw_write(
    void *context, void *opaque, const void *buffer, size_t length,
    size_t *out_written) {
    posix_stream_t *stream = opaque;
    maelys_sys_result_t result;
    (void)context;
    if (out_written) *out_written = 0u;
    if (!stream || stream->closed || !buffer || !length || !out_written) {
        return MAELYS_HTTP_IO_FAILED;
    }
    if (!stream->ready) {
        return stream->open_failed ? MAELYS_HTTP_IO_FAILED : MAELYS_HTTP_IO_WANT_WRITE;
    }
    if (stream->tls) return maelys_http_tls_session_write(
        stream->tls, buffer, length, out_written);
    result = maelys_sys_socket_send(
        stream->socket_handle, buffer, length, out_written);
    if (result == MAELYS_SYS_OK) return MAELYS_HTTP_IO_COMPLETE;
    if (result == MAELYS_SYS_ERR_WOULD_BLOCK) return MAELYS_HTTP_IO_WANT_WRITE;
    if (result == MAELYS_SYS_ERR_CLOSED) {
        remember(stream, "peer closed the connection before the request was sent");
        return MAELYS_HTTP_IO_FAILED;
    }
    if (result == MAELYS_SYS_ERR_RESET) {
        remember(stream, "connection reset by peer");
        return MAELYS_HTTP_IO_FAILED;
    }
    remember(stream, strerror(errno));
    return MAELYS_HTTP_IO_FAILED;
}

static maelys_http_result_t wait_stream(
    void *context, void *opaque, int want_read, int want_write,
    uint64_t deadline_ms) {
    posix_stream_t *stream = opaque;
    (void)context;
    if (!stream) return MAELYS_HTTP_ERR_ARGUMENT;
    if (!stream->ready) return progress_open(stream, deadline_ms);
    return stream_wait(stream, want_read, want_write, deadline_ms);
}
static void cancel_stream(void *context, void *opaque) {
    posix_stream_t *stream = opaque;
    (void)context;
    if (!stream) return;
    stream->cancelled = 1;
    if (stream->resolver_request) {
        (void)maelys_http_resolver_cancel_internal(stream->resolver_request);
    }
    if (stream->socket_handle) {
        (void)maelys_sys_socket_shutdown(stream->socket_handle, SHUT_RDWR);
    }
}
static void close_stream(void *context, void *opaque) {
    posix_stream_t *stream = opaque;
    (void)context;
    if (!stream || stream->closed) return;
    stream->closed = 1;
    stream->cancelled = 1;
    if (stream->resolver_request) {
        (void)maelys_http_resolver_cancel_internal(stream->resolver_request);
    }
    if (stream->tls) (void)maelys_http_tls_session_shutdown(stream->tls);
    if (stream->socket_handle) discard_connection(stream);
}
static const char *stream_error(void *context, const void *opaque) {
    const posix_stream_t *stream = opaque;
    (void)context;
    if (stream && stream->tls) return maelys_http_tls_session_error(stream->tls);
    return stream && stream->error[0] ? stream->error : "POSIX transport failure";
}
static void release_stream(void *context, void *opaque) {
    posix_stream_t *stream = opaque;
    (void)context;
    if (!stream) return;
    close_stream(NULL, stream);
    destroy_stream(stream);
}
static void release_context(void *opaque) {
    posix_context_t *context = opaque;
    if (!context) return;
    maelys_http_tls_provider_release(context->tls_provider);
    maelys_http_resolver_release_internal(context->resolver);
    free(context);
}

maelys_http_result_t maelys_http_posix_transport_create_with_resolver_internal(
    maelys_http_tls_provider_t *tls_provider,
    maelys_http_resolver_t *resolver,
    unsigned required_guarantees,
    maelys_http_transport_t **out_transport,
    char **out_error) {
    posix_context_t *context;
    maelys_http_transport_ops_t ops;
    if (out_transport) *out_transport = NULL;
    if (out_error) *out_error = NULL;
    if (!out_transport || !resolver ||
        (maelys_http_resolver_guarantees_internal(resolver) &
         required_guarantees) != required_guarantees) {
        if (out_error && resolver) {
            *out_error = strdup("resolver does not provide required guarantees");
        }
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    context = calloc(1u, sizeof(*context));
    if (!context) return MAELYS_HTTP_ERR_MEMORY;
    context->tls_provider = tls_provider;
    maelys_http_tls_provider_retain(tls_provider);
    context->resolver = resolver;
    maelys_http_resolver_retain_internal(resolver);
    memset(&ops, 0, sizeof(ops));
    ops.abi_version = MAELYS_HTTP_TRANSPORT_ABI_VERSION;
    ops.name = "posix";
    ops.open = open_stream;
    ops.read = raw_read;
    ops.write = raw_write;
    ops.wait = wait_stream;
    ops.cancel = cancel_stream;
    ops.close = close_stream;
    ops.last_error = stream_error;
    ops.stream_release = release_stream;
    {
        maelys_http_result_t result = maelys_http_transport_create(
            &ops, context, release_context, out_transport, out_error);
        if (result != MAELYS_HTTP_OK) release_context(context);
        return result;
    }
}

maelys_http_result_t maelys_http_posix_transport_create(
    maelys_http_tls_provider_t *tls_provider,
    maelys_http_transport_t **out_transport, char **out_error) {
    maelys_http_resolver_t *resolver = NULL;
    maelys_http_result_t result = maelys_http_posix_resolver_create_internal(
        &resolver, out_error);
    if (result != MAELYS_HTTP_OK) return result;
    result = maelys_http_posix_transport_create_with_resolver_internal(
        tls_provider, resolver, 0u, out_transport, out_error);
    maelys_http_resolver_release_internal(resolver);
    return result;
}
