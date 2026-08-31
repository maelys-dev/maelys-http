#ifndef MAELYS_HTTP_RESOLVER_INTERNAL_H
#define MAELYS_HTTP_RESOLVER_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "maelys/http.h"
#include "maelys/http_client.h"
#include "maelys/http_tls.h"

#define MAELYS_HTTP_RESOLVER_MAX_ADDRESSES 32u
#define MAELYS_HTTP_RESOLVER_MAX_HOST_BYTES 253u

enum {
    MAELYS_HTTP_RESOLVER_HARD_DEADLINE = 1u << 0,
    MAELYS_HTTP_RESOLVER_HARD_CANCEL = 1u << 1,
    MAELYS_HTTP_RESOLVER_SUPERVISED_PROCESS = 1u << 2
};

typedef enum maelys_http_resolver_family {
    MAELYS_HTTP_RESOLVER_IPV4 = 4,
    MAELYS_HTTP_RESOLVER_IPV6 = 6
} maelys_http_resolver_family_t;

typedef struct maelys_http_resolver_address {
    maelys_http_resolver_family_t family;
    unsigned char bytes[16];
    uint16_t port;
    uint32_t scope_id;
} maelys_http_resolver_address_t;

typedef struct maelys_http_resolver maelys_http_resolver_t;
typedef struct maelys_http_resolver_request maelys_http_resolver_request_t;

typedef struct maelys_http_resolver_ops {
    const char *name;
    unsigned guarantees;
    maelys_http_result_t (*start)(
        void *context,
        uint64_t request_id,
        const char *host,
        uint16_t port,
        uint64_t deadline_ms,
        void **out_request,
        char **out_error);
    int (*notification_fd)(void *context, const void *request);
    maelys_http_result_t (*take)(
        void *context,
        void *request,
        uint64_t *out_response_id,
        maelys_http_resolver_address_t *addresses,
        size_t capacity,
        size_t *out_count,
        char **out_error);
    maelys_http_result_t (*cancel)(void *context, void *request);
    void (*request_release)(void *context, void *request);
} maelys_http_resolver_ops_t;

/* Private until a process provider proves this contract. notification_fd is
 * borrowed and level-readable once take can complete without blocking. A
 * provider advertising HARD_CANCEL must make cancel concurrently interrupt
 * and reap the underlying operation before returning. A provider advertising
 * HARD_DEADLINE must not retain work beyond deadline_ms. */
maelys_http_result_t maelys_http_resolver_create_internal(
    const maelys_http_resolver_ops_t *ops,
    void *context,
    void (*release_context)(void *context),
    maelys_http_resolver_t **out_resolver,
    char **out_error);
void maelys_http_resolver_retain_internal(maelys_http_resolver_t *resolver);
void maelys_http_resolver_release_internal(maelys_http_resolver_t *resolver);
unsigned maelys_http_resolver_guarantees_internal(
    const maelys_http_resolver_t *resolver);
const char *maelys_http_resolver_name_internal(
    const maelys_http_resolver_t *resolver);

maelys_http_result_t maelys_http_resolver_start_internal(
    maelys_http_resolver_t *resolver,
    const char *host,
    uint16_t port,
    uint64_t deadline_ms,
    maelys_http_resolver_request_t **out_request,
    char **out_error);
int maelys_http_resolver_notification_fd_internal(
    const maelys_http_resolver_request_t *request);
maelys_http_result_t maelys_http_resolver_take_internal(
    maelys_http_resolver_request_t *request,
    maelys_http_resolver_address_t *addresses,
    size_t capacity,
    size_t *out_count,
    char **out_error);
maelys_http_result_t maelys_http_resolver_cancel_internal(
    maelys_http_resolver_request_t *request);
void maelys_http_resolver_request_release_internal(
    maelys_http_resolver_request_t *request);

maelys_http_result_t maelys_http_posix_resolver_create_internal(
    maelys_http_resolver_t **out_resolver,
    char **out_error);

maelys_http_result_t maelys_http_posix_transport_create_with_resolver_internal(
    maelys_http_tls_provider_t *tls_provider,
    maelys_http_resolver_t *resolver,
    unsigned required_guarantees,
    maelys_http_transport_t **out_transport,
    char **out_error);

#endif
