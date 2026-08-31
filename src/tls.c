#include "internal.h"

#include <stdlib.h>
#include <string.h>

maelys_http_result_t maelys_http_tls_provider_create(
    const maelys_http_tls_ops_t *ops, void *context,
    void (*release_context)(void *context),
    maelys_http_tls_provider_t **out_provider, char **out_error) {
    maelys_http_tls_provider_t *provider;
    if (out_provider) *out_provider = NULL;
    if (out_error) *out_error = NULL;
    if (!ops || !out_provider || ops->abi_version != MAELYS_HTTP_TLS_ABI_VERSION ||
        !ops->name || !ops->name[0] || !ops->session_create || !ops->handshake ||
        !ops->read || !ops->write || !ops->shutdown || !ops->last_error ||
        !ops->session_release) {
        maelys_http_internal_set_error(out_error, "invalid TLS provider ABI or operations");
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    provider = calloc(1u, sizeof(*provider));
    if (!provider) return MAELYS_HTTP_ERR_MEMORY;
    provider->name = maelys_http_internal_strdup(ops->name);
    if (!provider->name) {
        free(provider);
        return MAELYS_HTTP_ERR_MEMORY;
    }
    provider->ops = *ops;
    provider->ops.name = provider->name;
    provider->context = context;
    provider->release_context = release_context;
    atomic_init(&provider->references, 1u);
    *out_provider = provider;
    return MAELYS_HTTP_OK;
}

const char *maelys_http_tls_provider_name(
    const maelys_http_tls_provider_t *provider) {
    return provider ? provider->name : NULL;
}

void maelys_http_tls_provider_retain(maelys_http_tls_provider_t *provider) {
    if (provider) (void)atomic_fetch_add_explicit(
        &provider->references, 1u, memory_order_relaxed);
}

void maelys_http_tls_provider_release(maelys_http_tls_provider_t *provider) {
    if (!provider || atomic_fetch_sub_explicit(
            &provider->references, 1u, memory_order_acq_rel) != 1u) return;
    if (provider->release_context) provider->release_context(provider->context);
    free(provider->name);
    free(provider);
}

maelys_http_result_t maelys_http_tls_session_create_client(
    maelys_http_tls_provider_t *provider, int fd, const char *server_name,
    maelys_http_tls_session_t **out_session, char **out_error) {
    maelys_http_tls_session_t *session;
    void *implementation = NULL;
    maelys_http_result_t result;
    if (out_session) *out_session = NULL;
    if (out_error) *out_error = NULL;
    if (!provider || fd < 0 || !server_name || !server_name[0] || !out_session) {
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    session = calloc(1u, sizeof(*session));
    if (!session) return MAELYS_HTTP_ERR_MEMORY;
    result = provider->ops.session_create(provider->context, fd, server_name,
                                           &implementation, out_error);
    if (result != MAELYS_HTTP_OK) {
        free(session);
        return result;
    }
    if (!implementation) {
        free(session);
        maelys_http_internal_set_error(out_error, "TLS provider returned an empty session");
        return MAELYS_HTTP_ERR_TLS;
    }
    maelys_http_tls_provider_retain(provider);
    session->provider = provider;
    session->implementation = implementation;
    *out_session = session;
    return MAELYS_HTTP_OK;
}

maelys_http_io_step_t maelys_http_tls_session_handshake(
    maelys_http_tls_session_t *session) {
    return session ? session->provider->ops.handshake(
        session->provider->context, session->implementation) : MAELYS_HTTP_IO_FAILED;
}

maelys_http_io_step_t maelys_http_tls_session_read(
    maelys_http_tls_session_t *session, void *buffer, size_t capacity,
    size_t *out_read) {
    if (out_read) *out_read = 0u;
    return session ? session->provider->ops.read(
        session->provider->context, session->implementation,
        buffer, capacity, out_read) : MAELYS_HTTP_IO_FAILED;
}

maelys_http_io_step_t maelys_http_tls_session_write(
    maelys_http_tls_session_t *session, const void *buffer, size_t length,
    size_t *out_written) {
    if (out_written) *out_written = 0u;
    return session ? session->provider->ops.write(
        session->provider->context, session->implementation,
        buffer, length, out_written) : MAELYS_HTTP_IO_FAILED;
}

maelys_http_io_step_t maelys_http_tls_session_shutdown(
    maelys_http_tls_session_t *session) {
    return session ? session->provider->ops.shutdown(
        session->provider->context, session->implementation) : MAELYS_HTTP_IO_FAILED;
}

const char *maelys_http_tls_session_error(const maelys_http_tls_session_t *session) {
    return session ? session->provider->ops.last_error(
        session->provider->context, session->implementation) : "invalid TLS session";
}

void maelys_http_tls_session_release(maelys_http_tls_session_t *session) {
    if (!session) return;
    session->provider->ops.session_release(
        session->provider->context, session->implementation);
    maelys_http_tls_provider_release(session->provider);
    free(session);
}
