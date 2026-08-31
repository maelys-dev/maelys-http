#include "resolver_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "maelys/sys/fd.h"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return 1; \
} } while (0)

typedef enum fake_mode {
    FAKE_GOOD = 0,
    FAKE_WRONG_ID,
    FAKE_WRONG_FAMILY,
    FAKE_WRONG_PORT,
    FAKE_TOO_MANY,
    FAKE_WORKER_DIED,
    FAKE_PENDING
} fake_mode_t;

typedef struct fake_context {
    fake_mode_t mode;
    int cancel_calls;
    int release_calls;
} fake_context_t;

typedef struct fake_request {
    fake_context_t *context;
    uint64_t request_id;
    uint16_t port;
    int notification_fds[2];
} fake_request_t;

static maelys_http_result_t fake_start(
    void *opaque,
    uint64_t request_id,
    const char *host,
    uint16_t port,
    uint64_t deadline_ms,
    void **out_request,
    char **out_error) {
    fake_context_t *context = opaque;
    fake_request_t *request;
    unsigned char byte = 1u;
    ssize_t written;
    (void)deadline_ms;
    if (out_request) *out_request = NULL;
    if (out_error) *out_error = NULL;
    if (!context || !host || !port || !out_request) {
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    request = calloc(1u, sizeof(*request));
    if (!request) return MAELYS_HTTP_ERR_MEMORY;
    request->notification_fds[0] = -1;
    request->notification_fds[1] = -1;
    if (maelys_sys_pipe_cloexec(request->notification_fds) != MAELYS_SYS_OK ||
        maelys_sys_fd_set_nonblocking(request->notification_fds[0]) !=
            MAELYS_SYS_OK ||
        maelys_sys_fd_set_nonblocking(request->notification_fds[1]) !=
            MAELYS_SYS_OK) {
        (void)maelys_sys_fd_close(&request->notification_fds[0]);
        (void)maelys_sys_fd_close(&request->notification_fds[1]);
        free(request);
        return MAELYS_HTTP_ERR_IO;
    }
    request->context = context;
    request->request_id = request_id;
    request->port = port;
    if (context->mode != FAKE_PENDING) {
        do {
            written = write(request->notification_fds[1], &byte, sizeof(byte));
        } while (written < 0 && errno == EINTR);
        if (written != (ssize_t)sizeof(byte)) {
            (void)maelys_sys_fd_close(&request->notification_fds[0]);
            (void)maelys_sys_fd_close(&request->notification_fds[1]);
            free(request);
            return MAELYS_HTTP_ERR_IO;
        }
    }
    *out_request = request;
    return MAELYS_HTTP_OK;
}

static int fake_notification_fd(void *opaque, const void *request_opaque) {
    const fake_request_t *request = request_opaque;
    (void)opaque;
    return request ? request->notification_fds[0] : -1;
}

static maelys_http_result_t fake_take(
    void *opaque,
    void *request_opaque,
    uint64_t *out_response_id,
    maelys_http_resolver_address_t *addresses,
    size_t capacity,
    size_t *out_count,
    char **out_error) {
    fake_request_t *request = request_opaque;
    fake_context_t *context = opaque;
    unsigned char byte;
    (void)capacity;
    if (out_response_id) *out_response_id = 0u;
    if (out_count) *out_count = 0u;
    if (out_error) *out_error = NULL;
    if (!request || !context || !out_response_id || !addresses || !out_count) {
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    if (context->mode == FAKE_PENDING) return MAELYS_HTTP_AGAIN;
    if (read(request->notification_fds[0], &byte, sizeof(byte)) < 0 &&
        errno != EAGAIN && errno != EWOULDBLOCK) {
        return MAELYS_HTTP_ERR_IO;
    }
    if (context->mode == FAKE_WORKER_DIED) return MAELYS_HTTP_ERR_IO;
    memset(addresses, 0, sizeof(*addresses));
    addresses[0].family = context->mode == FAKE_WRONG_FAMILY ?
        (maelys_http_resolver_family_t)5 : MAELYS_HTTP_RESOLVER_IPV4;
    addresses[0].bytes[0] = 127u;
    addresses[0].bytes[3] = 1u;
    addresses[0].port = context->mode == FAKE_WRONG_PORT ?
        (uint16_t)(request->port + 1u) : request->port;
    *out_response_id = context->mode == FAKE_WRONG_ID ?
        request->request_id + UINT64_C(1) : request->request_id;
    *out_count = context->mode == FAKE_TOO_MANY ? capacity + 1u : 1u;
    return MAELYS_HTTP_OK;
}

static maelys_http_result_t fake_cancel(void *opaque, void *request_opaque) {
    fake_context_t *context = opaque;
    if (!context || !request_opaque) return MAELYS_HTTP_ERR_ARGUMENT;
    ++context->cancel_calls;
    return MAELYS_HTTP_OK;
}

static void fake_request_release(void *opaque, void *request_opaque) {
    fake_context_t *context = opaque;
    fake_request_t *request = request_opaque;
    if (!request) return;
    (void)maelys_sys_fd_close(&request->notification_fds[0]);
    (void)maelys_sys_fd_close(&request->notification_fds[1]);
    if (context) ++context->release_calls;
    free(request);
}

static maelys_http_resolver_ops_t fake_ops(unsigned guarantees) {
    maelys_http_resolver_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.name = "fake-contract-resolver";
    ops.guarantees = guarantees;
    ops.start = fake_start;
    ops.notification_fd = fake_notification_fd;
    ops.take = fake_take;
    ops.cancel = fake_cancel;
    ops.request_release = fake_request_release;
    return ops;
}

static int exercise_mode(fake_mode_t mode, maelys_http_result_t expected) {
    fake_context_t context = {.mode = mode, .cancel_calls = 0, .release_calls = 0};
    maelys_http_resolver_ops_t ops = fake_ops(
        MAELYS_HTTP_RESOLVER_HARD_DEADLINE |
        MAELYS_HTTP_RESOLVER_HARD_CANCEL |
        MAELYS_HTTP_RESOLVER_SUPERVISED_PROCESS);
    maelys_http_resolver_t *resolver = NULL;
    maelys_http_resolver_request_t *request = NULL;
    maelys_http_resolver_address_t addresses[MAELYS_HTTP_RESOLVER_MAX_ADDRESSES];
    size_t count = 0u;
    char *error = NULL;
    CHECK(maelys_http_resolver_create_internal(
        &ops, &context, NULL, &resolver, &error) == MAELYS_HTTP_OK);
    CHECK(!error);
    CHECK(!strcmp(maelys_http_resolver_name_internal(resolver),
                  "fake-contract-resolver"));
    CHECK(maelys_http_resolver_guarantees_internal(resolver) == ops.guarantees);
    CHECK(maelys_http_resolver_start_internal(
        resolver, "example.test", 443u, UINT64_C(12345),
        &request, &error) == MAELYS_HTTP_OK);
    CHECK(request && !error);
    CHECK(maelys_http_resolver_notification_fd_internal(request) >= 0);
    CHECK(maelys_http_resolver_take_internal(
        request, addresses, MAELYS_HTTP_RESOLVER_MAX_ADDRESSES,
        &count, &error) == expected);
    if (expected == MAELYS_HTTP_OK) {
        CHECK(count == 1u);
        CHECK(addresses[0].family == MAELYS_HTTP_RESOLVER_IPV4);
        CHECK(addresses[0].port == 443u);
    }
    free(error);
    maelys_http_resolver_request_release_internal(request);
    CHECK(context.release_calls == 1);
    maelys_http_resolver_release_internal(resolver);
    return 0;
}

int main(void) {
    fake_context_t context = {0};
    maelys_http_resolver_ops_t invalid = fake_ops(
        MAELYS_HTTP_RESOLVER_SUPERVISED_PROCESS);
    maelys_http_resolver_ops_t ops = fake_ops(0u);
    maelys_http_resolver_t *resolver = NULL;
    maelys_http_resolver_request_t *request = NULL;
    maelys_http_transport_t *transport = NULL;
    maelys_http_resolver_t *compat_resolver = NULL;
    char long_host[MAELYS_HTTP_RESOLVER_MAX_HOST_BYTES + 2u];
    char *error = NULL;

    CHECK(maelys_http_resolver_create_internal(
        &invalid, &context, NULL, &resolver, &error) == MAELYS_HTTP_ERR_ARGUMENT);
    CHECK(!resolver && error);
    free(error);
    error = NULL;
    CHECK(exercise_mode(FAKE_GOOD, MAELYS_HTTP_OK) == 0);
    CHECK(exercise_mode(FAKE_WRONG_ID, MAELYS_HTTP_ERR_IO) == 0);
    CHECK(exercise_mode(FAKE_WRONG_FAMILY, MAELYS_HTTP_ERR_IO) == 0);
    CHECK(exercise_mode(FAKE_WRONG_PORT, MAELYS_HTTP_ERR_IO) == 0);
    CHECK(exercise_mode(FAKE_TOO_MANY, MAELYS_HTTP_ERR_IO) == 0);
    CHECK(exercise_mode(FAKE_WORKER_DIED, MAELYS_HTTP_ERR_IO) == 0);
    CHECK(maelys_http_posix_resolver_create_internal(
        &compat_resolver, &error) == MAELYS_HTTP_OK);
    CHECK(compat_resolver && !error);
    CHECK(maelys_http_resolver_guarantees_internal(compat_resolver) == 0u);
    CHECK(!strcmp(maelys_http_resolver_name_internal(compat_resolver),
                  "posix-getaddrinfo-compat"));
    maelys_http_resolver_release_internal(compat_resolver);

    CHECK(maelys_http_resolver_create_internal(
        &ops, &context, NULL, &resolver, &error) == MAELYS_HTTP_OK);
    CHECK(maelys_http_posix_transport_create_with_resolver_internal(
        NULL, resolver, MAELYS_HTTP_RESOLVER_HARD_CANCEL,
        &transport, &error) == MAELYS_HTTP_ERR_ARGUMENT);
    CHECK(!transport && error);
    free(error);
    error = NULL;
    CHECK(maelys_http_posix_transport_create_with_resolver_internal(
        NULL, resolver, 0u, &transport, &error) == MAELYS_HTTP_OK);
    CHECK(transport && !error);
    maelys_http_transport_release(transport);
    transport = NULL;
    memset(long_host, 'a', sizeof(long_host));
    long_host[sizeof(long_host) - 1u] = '\0';
    CHECK(maelys_http_resolver_start_internal(
        resolver, long_host, 443u, UINT64_C(1),
        &request, &error) == MAELYS_HTTP_ERR_ARGUMENT);
    CHECK(!request);
    context.mode = FAKE_PENDING;
    CHECK(maelys_http_resolver_start_internal(
        resolver, "pending.test", 443u, UINT64_C(1),
        &request, &error) == MAELYS_HTTP_OK);
    CHECK(maelys_http_resolver_take_internal(
        request, (maelys_http_resolver_address_t[1]){{0}}, 1u,
        &(size_t){0u}, &error) == MAELYS_HTTP_AGAIN);
    CHECK(maelys_http_resolver_cancel_internal(request) == MAELYS_HTTP_OK);
    CHECK(context.cancel_calls == 1);
    maelys_http_resolver_request_release_internal(request);
    maelys_http_resolver_release_internal(resolver);
    free(error);
    puts("test_resolver_internal: ok");
    return 0;
}
