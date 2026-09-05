#include "resolver_internal.h"

#include "internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "maelys/sys/wakeup.h"

#define RESOLVER_WORKERS 4u
#define RESOLVER_QUEUE_CAPACITY 64u

typedef struct posix_resolver_request {
    atomic_uint references;
    pthread_mutex_t lock;
    int lock_initialized;
    maelys_sys_wakeup_t *wakeup;
    uint64_t request_id;
    char *host;
    char service[6];
    uint16_t port;
    int cancelled;
    int complete;
    maelys_http_result_t result;
    maelys_http_resolver_address_t addresses[MAELYS_HTTP_RESOLVER_MAX_ADDRESSES];
    size_t address_count;
    char error[128];
} posix_resolver_request_t;

typedef struct resolver_pool {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    posix_resolver_request_t *queue[RESOLVER_QUEUE_CAPACITY];
    size_t head;
    size_t count;
    size_t workers;
} resolver_pool_t;

static resolver_pool_t resolver_pool = {
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER,
    {NULL}, 0u, 0u, 0u
};
static pthread_once_t resolver_pool_once = PTHREAD_ONCE_INIT;

static void request_release_reference(posix_resolver_request_t *request) {
    if (!request || atomic_fetch_sub_explicit(
            &request->references, 1u, memory_order_acq_rel) != 1u) return;
    maelys_sys_wakeup_destroy(request->wakeup);
    (void)pthread_mutex_destroy(&request->lock);
    free(request->host);
    memset(request, 0, sizeof(*request));
    free(request);
}

static void convert_addresses(
    posix_resolver_request_t *request,
    const struct addrinfo *addresses) {
    const struct addrinfo *cursor;
    size_t count = 0u;
    int overflow = 0;
    for (cursor = addresses; cursor; cursor = cursor->ai_next) {
        maelys_http_resolver_address_t converted;
        memset(&converted, 0, sizeof(converted));
        if (cursor->ai_family == AF_INET &&
            cursor->ai_addrlen >= (socklen_t)sizeof(struct sockaddr_in)) {
            const struct sockaddr_in *address =
                (const struct sockaddr_in *)cursor->ai_addr;
            converted.family = MAELYS_HTTP_RESOLVER_IPV4;
            memcpy(converted.bytes, &address->sin_addr, 4u);
        } else if (cursor->ai_family == AF_INET6 &&
                   cursor->ai_addrlen >= (socklen_t)sizeof(struct sockaddr_in6)) {
            const struct sockaddr_in6 *address =
                (const struct sockaddr_in6 *)cursor->ai_addr;
            converted.family = MAELYS_HTTP_RESOLVER_IPV6;
            memcpy(converted.bytes, &address->sin6_addr, 16u);
            converted.scope_id = address->sin6_scope_id;
        } else {
            continue;
        }
        converted.port = request->port;
        if (count == MAELYS_HTTP_RESOLVER_MAX_ADDRESSES) {
            overflow = 1;
            break;
        }
        request->addresses[count++] = converted;
    }
    if (overflow) {
        request->result = MAELYS_HTTP_ERR_LIMIT;
        (void)snprintf(request->error, sizeof(request->error),
                       "%s", "DNS address result limit exceeded");
    } else if (!count) {
        request->result = MAELYS_HTTP_ERR_IO;
        (void)snprintf(request->error, sizeof(request->error),
                       "%s", "DNS returned no supported numeric address");
    } else {
        request->address_count = count;
        request->result = MAELYS_HTTP_OK;
    }
}

static void resolve_one(posix_resolver_request_t *request) {
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    int status;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    status = getaddrinfo(request->host, request->service, &hints, &addresses);
    (void)pthread_mutex_lock(&request->lock);
    if (!request->cancelled) {
        if (status == 0) convert_addresses(request, addresses);
        else {
            request->result = MAELYS_HTTP_ERR_IO;
            (void)snprintf(request->error, sizeof(request->error),
                           "DNS resolution failed: %s", gai_strerror(status));
        }
        request->complete = 1;
    }
    (void)pthread_mutex_unlock(&request->lock);
    if (addresses) freeaddrinfo(addresses);
    /* The wakeup is the cross-thread edge: signal is safe from a worker and
     * a saturated counter still leaves the wakeup pending. */
    (void)maelys_sys_wakeup_signal(request->wakeup);
    request_release_reference(request);
}

static void *resolver_pool_worker(void *unused) {
    (void)unused;
    for (;;) {
        posix_resolver_request_t *request;
        (void)pthread_mutex_lock(&resolver_pool.lock);
        while (!resolver_pool.count) {
            (void)pthread_cond_wait(&resolver_pool.ready, &resolver_pool.lock);
        }
        request = resolver_pool.queue[resolver_pool.head];
        resolver_pool.queue[resolver_pool.head] = NULL;
        resolver_pool.head = (resolver_pool.head + 1u) % RESOLVER_QUEUE_CAPACITY;
        --resolver_pool.count;
        (void)pthread_mutex_unlock(&resolver_pool.lock);
        resolve_one(request);
    }
    return NULL;
}

static void initialize_pool(void) {
    pthread_attr_t attributes;
    size_t index;
    if (pthread_attr_init(&attributes) != 0) return;
    if (pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED) != 0) {
        (void)pthread_attr_destroy(&attributes);
        return;
    }
    for (index = 0u; index < RESOLVER_WORKERS; ++index) {
        pthread_t thread;
        if (pthread_create(&thread, &attributes,
                           resolver_pool_worker, NULL) == 0) {
            ++resolver_pool.workers;
        }
    }
    (void)pthread_attr_destroy(&attributes);
}

static int submit(posix_resolver_request_t *request) {
    size_t tail;
    (void)pthread_once(&resolver_pool_once, initialize_pool);
    (void)pthread_mutex_lock(&resolver_pool.lock);
    if (!resolver_pool.workers ||
        resolver_pool.count == RESOLVER_QUEUE_CAPACITY) {
        (void)pthread_mutex_unlock(&resolver_pool.lock);
        return 0;
    }
    tail = (resolver_pool.head + resolver_pool.count) % RESOLVER_QUEUE_CAPACITY;
    resolver_pool.queue[tail] = request;
    ++resolver_pool.count;
    (void)pthread_cond_signal(&resolver_pool.ready);
    (void)pthread_mutex_unlock(&resolver_pool.lock);
    return 1;
}

static maelys_http_result_t posix_start(
    void *context,
    uint64_t request_id,
    const char *host,
    uint16_t port,
    uint64_t deadline_ms,
    void **out_request,
    char **out_error) {
    posix_resolver_request_t *request;
    int service_length;
    (void)context;
    (void)deadline_ms;
    if (out_request) *out_request = NULL;
    if (out_error) *out_error = NULL;
    if (!host || !port || !out_request) return MAELYS_HTTP_ERR_ARGUMENT;
    request = calloc(1u, sizeof(*request));
    if (!request) return MAELYS_HTTP_ERR_MEMORY;
    request->host = maelys_http_internal_strdup(host);
    request->request_id = request_id;
    request->port = port;
    request->result = MAELYS_HTTP_AGAIN;
    service_length = snprintf(request->service, sizeof(request->service),
                              "%u", (unsigned)port);
    if (!request->host || service_length <= 0 ||
        (size_t)service_length >= sizeof(request->service)) {
        free(request->host);
        free(request);
        return MAELYS_HTTP_ERR_MEMORY;
    }
    if (pthread_mutex_init(&request->lock, NULL) != 0) {
        free(request->host);
        free(request);
        return MAELYS_HTTP_ERR_IO;
    }
    request->lock_initialized = 1;
    if (maelys_sys_wakeup_create(&request->wakeup) != MAELYS_SYS_OK) {
        (void)pthread_mutex_destroy(&request->lock);
        free(request->host);
        free(request);
        return MAELYS_HTTP_ERR_IO;
    }
    atomic_init(&request->references, 2u);
    if (!submit(request)) {
        atomic_store_explicit(&request->references, 1u, memory_order_relaxed);
        request_release_reference(request);
        maelys_http_internal_set_error(
            out_error, "DNS resolver queue is unavailable or full");
        return MAELYS_HTTP_ERR_LIMIT;
    }
    *out_request = request;
    return MAELYS_HTTP_OK;
}

static int posix_notification_fd(void *context, const void *opaque) {
    const posix_resolver_request_t *request = opaque;
    (void)context;
    return request ? maelys_sys_wakeup_fd(request->wakeup) : -1;
}

static maelys_http_result_t posix_take(
    void *context,
    void *opaque,
    uint64_t *out_response_id,
    maelys_http_resolver_address_t *addresses,
    size_t capacity,
    size_t *out_count,
    char **out_error) {
    posix_resolver_request_t *request = opaque;
    maelys_http_result_t result;
    (void)context;
    if (out_response_id) *out_response_id = 0u;
    if (out_count) *out_count = 0u;
    if (out_error) *out_error = NULL;
    if (!request || !out_response_id || !addresses || !capacity || !out_count) {
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    /* Consuming with nothing pending is a success; the state below decides. */
    if (maelys_sys_wakeup_consume(request->wakeup) != MAELYS_SYS_OK) {
        return MAELYS_HTTP_ERR_IO;
    }
    (void)pthread_mutex_lock(&request->lock);
    if (request->cancelled) result = MAELYS_HTTP_ERR_CANCELLED;
    else if (!request->complete) result = MAELYS_HTTP_AGAIN;
    else if (request->result != MAELYS_HTTP_OK) {
        result = request->result;
        maelys_http_internal_set_error(out_error, request->error);
    } else if (request->address_count > capacity) {
        result = MAELYS_HTTP_ERR_LIMIT;
    } else {
        memcpy(addresses, request->addresses,
               request->address_count * sizeof(*addresses));
        *out_count = request->address_count;
        *out_response_id = request->request_id;
        result = MAELYS_HTTP_OK;
    }
    (void)pthread_mutex_unlock(&request->lock);
    return result;
}

static maelys_http_result_t posix_cancel(void *context, void *opaque) {
    posix_resolver_request_t *request = opaque;
    (void)context;
    if (!request) return MAELYS_HTTP_ERR_ARGUMENT;
    (void)pthread_mutex_lock(&request->lock);
    request->cancelled = 1;
    (void)pthread_mutex_unlock(&request->lock);
    return MAELYS_HTTP_OK;
}

static void posix_request_release(void *context, void *opaque) {
    (void)context;
    request_release_reference(opaque);
}

maelys_http_result_t maelys_http_posix_resolver_create_internal(
    maelys_http_resolver_t **out_resolver,
    char **out_error) {
    maelys_http_resolver_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.name = "posix-getaddrinfo-compat";
    ops.guarantees = 0u;
    ops.start = posix_start;
    ops.notification_fd = posix_notification_fd;
    ops.take = posix_take;
    ops.cancel = posix_cancel;
    ops.request_release = posix_request_release;
    return maelys_http_resolver_create_internal(
        &ops, NULL, NULL, out_resolver, out_error);
}
