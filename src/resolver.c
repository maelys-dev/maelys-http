#include "resolver_internal.h"

#include "internal.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct maelys_http_resolver {
    atomic_uint references;
    char *name;
    maelys_http_resolver_ops_t ops;
    void *context;
    void (*release_context)(void *context);
};

struct maelys_http_resolver_request {
    maelys_http_resolver_t *resolver;
    void *provider_request;
    uint64_t request_id;
    uint16_t port;
    int taken;
};

static atomic_uint_fast64_t next_request_id = ATOMIC_VAR_INIT(UINT64_C(1));

static int valid_guarantees(unsigned guarantees) {
    const unsigned known = MAELYS_HTTP_RESOLVER_HARD_DEADLINE |
        MAELYS_HTTP_RESOLVER_HARD_CANCEL |
        MAELYS_HTTP_RESOLVER_SUPERVISED_PROCESS;
    return (guarantees & ~known) == 0u &&
        (!(guarantees & MAELYS_HTTP_RESOLVER_SUPERVISED_PROCESS) ||
         (guarantees & (MAELYS_HTTP_RESOLVER_HARD_DEADLINE |
                        MAELYS_HTTP_RESOLVER_HARD_CANCEL)) ==
             (MAELYS_HTTP_RESOLVER_HARD_DEADLINE |
              MAELYS_HTTP_RESOLVER_HARD_CANCEL));
}

maelys_http_result_t maelys_http_resolver_create_internal(
    const maelys_http_resolver_ops_t *ops,
    void *context,
    void (*release_context)(void *context),
    maelys_http_resolver_t **out_resolver,
    char **out_error) {
    maelys_http_resolver_t *resolver;
    if (out_resolver) *out_resolver = NULL;
    if (out_error) *out_error = NULL;
    if (!ops || !out_resolver || !ops->name || !ops->name[0] ||
        !valid_guarantees(ops->guarantees) || !ops->start ||
        !ops->notification_fd || !ops->take || !ops->cancel ||
        !ops->request_release) {
        maelys_http_internal_set_error(out_error, "invalid resolver operations");
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    resolver = calloc(1u, sizeof(*resolver));
    if (!resolver) return MAELYS_HTTP_ERR_MEMORY;
    resolver->name = maelys_http_internal_strdup(ops->name);
    if (!resolver->name) {
        free(resolver);
        return MAELYS_HTTP_ERR_MEMORY;
    }
    resolver->ops = *ops;
    resolver->ops.name = resolver->name;
    resolver->context = context;
    resolver->release_context = release_context;
    atomic_init(&resolver->references, 1u);
    *out_resolver = resolver;
    return MAELYS_HTTP_OK;
}

void maelys_http_resolver_retain_internal(maelys_http_resolver_t *resolver) {
    if (resolver) (void)atomic_fetch_add_explicit(
        &resolver->references, 1u, memory_order_relaxed);
}

void maelys_http_resolver_release_internal(maelys_http_resolver_t *resolver) {
    if (!resolver || atomic_fetch_sub_explicit(
            &resolver->references, 1u, memory_order_acq_rel) != 1u) return;
    if (resolver->release_context) resolver->release_context(resolver->context);
    free(resolver->name);
    free(resolver);
}

unsigned maelys_http_resolver_guarantees_internal(
    const maelys_http_resolver_t *resolver) {
    return resolver ? resolver->ops.guarantees : 0u;
}

const char *maelys_http_resolver_name_internal(
    const maelys_http_resolver_t *resolver) {
    return resolver ? resolver->name : NULL;
}

maelys_http_result_t maelys_http_resolver_start_internal(
    maelys_http_resolver_t *resolver,
    const char *host,
    uint16_t port,
    uint64_t deadline_ms,
    maelys_http_resolver_request_t **out_request,
    char **out_error) {
    maelys_http_resolver_request_t *request;
    void *provider_request = NULL;
    uint64_t request_id;
    maelys_http_result_t result;
    size_t host_length = host ? strlen(host) : 0u;
    if (out_request) *out_request = NULL;
    if (out_error) *out_error = NULL;
    if (!resolver || !host || !host_length ||
        host_length > MAELYS_HTTP_RESOLVER_MAX_HOST_BYTES || !port ||
        !out_request) {
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    request_id = atomic_fetch_add_explicit(
        &next_request_id, UINT64_C(1), memory_order_relaxed);
    if (!request_id) request_id = atomic_fetch_add_explicit(
        &next_request_id, UINT64_C(1), memory_order_relaxed);
    result = resolver->ops.start(resolver->context, request_id, host, port,
                                 deadline_ms, &provider_request, out_error);
    if (result != MAELYS_HTTP_OK) {
        if (provider_request) resolver->ops.request_release(
            resolver->context, provider_request);
        return result;
    }
    if (!provider_request ||
        resolver->ops.notification_fd(resolver->context, provider_request) < 0) {
        if (provider_request) resolver->ops.request_release(
            resolver->context, provider_request);
        maelys_http_internal_set_error(
            out_error, "resolver returned an invalid request");
        return MAELYS_HTTP_ERR_IO;
    }
    request = calloc(1u, sizeof(*request));
    if (!request) {
        (void)resolver->ops.cancel(resolver->context, provider_request);
        resolver->ops.request_release(resolver->context, provider_request);
        return MAELYS_HTTP_ERR_MEMORY;
    }
    request->resolver = resolver;
    request->provider_request = provider_request;
    request->request_id = request_id;
    request->port = port;
    maelys_http_resolver_retain_internal(resolver);
    *out_request = request;
    return MAELYS_HTTP_OK;
}

int maelys_http_resolver_notification_fd_internal(
    const maelys_http_resolver_request_t *request) {
    return request ? request->resolver->ops.notification_fd(
        request->resolver->context, request->provider_request) : -1;
}

static int address_valid(
    const maelys_http_resolver_address_t *address,
    uint16_t expected_port) {
    return address && address->port == expected_port &&
        (address->family == MAELYS_HTTP_RESOLVER_IPV4 ||
         address->family == MAELYS_HTTP_RESOLVER_IPV6) &&
        (address->family == MAELYS_HTTP_RESOLVER_IPV6 || !address->scope_id);
}

maelys_http_result_t maelys_http_resolver_take_internal(
    maelys_http_resolver_request_t *request,
    maelys_http_resolver_address_t *addresses,
    size_t capacity,
    size_t *out_count,
    char **out_error) {
    uint64_t response_id = 0u;
    size_t count = 0u;
    size_t index;
    maelys_http_result_t result;
    if (out_count) *out_count = 0u;
    if (out_error) *out_error = NULL;
    if (!request || request->taken || !addresses || !capacity || !out_count ||
        capacity > MAELYS_HTTP_RESOLVER_MAX_ADDRESSES) {
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    result = request->resolver->ops.take(
        request->resolver->context, request->provider_request, &response_id,
        addresses, capacity, &count, out_error);
    if (result == MAELYS_HTTP_AGAIN) return result;
    request->taken = 1;
    if (result != MAELYS_HTTP_OK) return result;
    if (response_id != request->request_id || !count || count > capacity) {
        maelys_http_internal_set_error(
            out_error, "resolver response identity or count is invalid");
        return MAELYS_HTTP_ERR_IO;
    }
    for (index = 0u; index < count; ++index) {
        if (!address_valid(&addresses[index], request->port)) {
            maelys_http_internal_set_error(
                out_error, "resolver returned an invalid numeric address");
            return MAELYS_HTTP_ERR_IO;
        }
    }
    *out_count = count;
    return MAELYS_HTTP_OK;
}

maelys_http_result_t maelys_http_resolver_cancel_internal(
    maelys_http_resolver_request_t *request) {
    return request ? request->resolver->ops.cancel(
        request->resolver->context, request->provider_request) :
        MAELYS_HTTP_ERR_ARGUMENT;
}

void maelys_http_resolver_request_release_internal(
    maelys_http_resolver_request_t *request) {
    if (!request) return;
    request->resolver->ops.request_release(
        request->resolver->context, request->provider_request);
    maelys_http_resolver_release_internal(request->resolver);
    memset(request, 0, sizeof(*request));
    free(request);
}
