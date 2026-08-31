#ifndef MAELYS_HTTP_TLS_H
#define MAELYS_HTTP_TLS_H

#include <stddef.h>

#include "maelys/http.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAELYS_HTTP_TLS_ABI_VERSION 1u

typedef struct maelys_http_tls_provider maelys_http_tls_provider_t;
typedef struct maelys_http_tls_session maelys_http_tls_session_t;

/*
 * Backend-neutral, owner-thread-confined, nonblocking TLS seam. Retain/release
 * may move an idle provider, but sessions and a provider context are not used
 * concurrently. The provider
 * borrows fd and never closes it. server_name is mandatory for client
 * sessions and is used for both SNI and certificate hostname verification.
 */
typedef struct maelys_http_tls_ops {
    unsigned abi_version;
    const char *name;
    maelys_http_result_t (*session_create)(
        void *context,
        int fd,
        const char *server_name,
        void **out_session,
        char **out_error);
    maelys_http_io_step_t (*handshake)(void *context, void *session);
    maelys_http_io_step_t (*read)(
        void *context,
        void *session,
        void *buffer,
        size_t capacity,
        size_t *out_read);
    maelys_http_io_step_t (*write)(
        void *context,
        void *session,
        const void *buffer,
        size_t length,
        size_t *out_written);
    maelys_http_io_step_t (*shutdown)(void *context, void *session);
    const char *(*last_error)(void *context, const void *session);
    void (*session_release)(void *context, void *session);
} maelys_http_tls_ops_t;

/*
 * Provider ownership contract:
 * - session_create returns OK with a non-NULL owned session, or a non-OK
 *   result with *out_session NULL;
 * - non-NULL *out_error is malloc-compatible storage transferred to the
 *   caller; borrowed diagnostics are returned only by last_error;
 * - I/O operations are nonblocking. IO_COMPLETE from read/write transfers
 *   1..requested bytes; all other steps leave the byte output zero;
 * - read returns IO_CLOSED only after an authenticated TLS close_notify;
 *   transport EOF without close_notify returns IO_FAILED so close-delimited
 *   HTTP cannot accept a truncated TLS record stream;
 * - shutdown is idempotent, the provider never closes fd, and
 *   session_release is called exactly once;
 * - last_error is non-secret borrowed storage valid until session_release.
 */

maelys_http_result_t maelys_http_tls_provider_create(
    const maelys_http_tls_ops_t *ops,
    void *context,
    void (*release_context)(void *context),
    maelys_http_tls_provider_t **out_provider,
    char **out_error);
const char *maelys_http_tls_provider_name(
    const maelys_http_tls_provider_t *provider);
void maelys_http_tls_provider_retain(maelys_http_tls_provider_t *provider);
void maelys_http_tls_provider_release(maelys_http_tls_provider_t *provider);

maelys_http_result_t maelys_http_tls_session_create_client(
    maelys_http_tls_provider_t *provider,
    int fd,
    const char *server_name,
    maelys_http_tls_session_t **out_session,
    char **out_error);
maelys_http_io_step_t maelys_http_tls_session_handshake(
    maelys_http_tls_session_t *session);
maelys_http_io_step_t maelys_http_tls_session_read(
    maelys_http_tls_session_t *session,
    void *buffer,
    size_t capacity,
    size_t *out_read);
maelys_http_io_step_t maelys_http_tls_session_write(
    maelys_http_tls_session_t *session,
    const void *buffer,
    size_t length,
    size_t *out_written);
maelys_http_io_step_t maelys_http_tls_session_shutdown(
    maelys_http_tls_session_t *session);
const char *maelys_http_tls_session_error(
    const maelys_http_tls_session_t *session);
void maelys_http_tls_session_release(maelys_http_tls_session_t *session);

#ifdef __cplusplus
}
#endif

#endif
