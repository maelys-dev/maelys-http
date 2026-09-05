#include "maelys/http_tls_modules.h"

#include <errno.h>
#include <limits.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>
#if MBEDTLS_VERSION_MAJOR < 4
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#else
#include <psa/crypto.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

typedef struct provider_context {
    mbedtls_ssl_config config;
    mbedtls_x509_crt authorities;
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
#endif
} provider_context_t;

typedef struct provider_session {
    mbedtls_ssl_context ssl;
    int fd;
    char error[192];
} provider_session_t;

#if MBEDTLS_VERSION_MAJOR >= 4
static pthread_once_t psa_once = PTHREAD_ONCE_INIT;
static psa_status_t psa_status = PSA_ERROR_GENERIC_ERROR;
static void initialize_psa(void) { psa_status = psa_crypto_init(); }
#endif

static char *copy_error(const char *prefix, int code) {
    char detail[128];
    size_t length;
    char *message;
    mbedtls_strerror(code, detail, sizeof(detail));
    length = strlen(prefix) + strlen(detail) + 3u;
    message = malloc(length);
    if (message) (void)snprintf(message, length, "%s: %s", prefix, detail);
    return message;
}

static int socket_send(void *opaque, const unsigned char *buffer, size_t length) {
    provider_session_t *session = opaque;
    size_t bounded = length > (size_t)INT_MAX ? (size_t)INT_MAX : length;
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#elif defined(SO_NOSIGPIPE)
    {
        int enabled = 1;
        if (setsockopt(session->fd, SOL_SOCKET, SO_NOSIGPIPE,
                       &enabled, sizeof(enabled)) != 0) {
            return MBEDTLS_ERR_NET_SEND_FAILED;
        }
    }
#endif
    {
        ssize_t sent = send(session->fd, buffer, bounded, flags);
        if (sent >= 0) return (int)sent;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int socket_receive(void *opaque, unsigned char *buffer, size_t length) {
    provider_session_t *session = opaque;
    size_t bounded = length > (size_t)INT_MAX ? (size_t)INT_MAX : length;
    ssize_t received = recv(session->fd, buffer, bounded, 0);
    if (received >= 0) return (int)received;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

static void remember_error(provider_session_t *session, int code) {
    mbedtls_strerror(code, session->error, sizeof(session->error));
    if (!session->error[0]) {
        (void)snprintf(session->error, sizeof(session->error),
                       "Mbed TLS error %d", code);
    }
}

static maelys_http_io_step_t translate(
    provider_session_t *session, int result, int zero_is_closed) {
    if (result > 0 || (result == 0 && !zero_is_closed)) return MAELYS_HTTP_IO_COMPLETE;
    if (result == MBEDTLS_ERR_SSL_WANT_READ) return MAELYS_HTTP_IO_WANT_READ;
    if (result == MBEDTLS_ERR_SSL_WANT_WRITE) return MAELYS_HTTP_IO_WANT_WRITE;
    if (result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return MAELYS_HTTP_IO_CLOSED;
    if (zero_is_closed && result == 0) {
        (void)snprintf(session->error, sizeof(session->error),
                       "TLS connection ended without close_notify");
        return MAELYS_HTTP_IO_FAILED;
    }
    remember_error(session, result);
    return MAELYS_HTTP_IO_FAILED;
}

static maelys_http_result_t session_create(
    void *opaque, int fd, const char *server_name, void **out_session,
    char **out_error) {
    provider_context_t *context = opaque;
    provider_session_t *session;
    int result;
    if (out_session) *out_session = NULL;
    if (out_error) *out_error = NULL;
    if (!context || fd < 0 || !server_name || !server_name[0] || !out_session) {
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    session = calloc(1u, sizeof(*session));
    if (!session) return MAELYS_HTTP_ERR_MEMORY;
    session->fd = fd;
    mbedtls_ssl_init(&session->ssl);
    result = mbedtls_ssl_setup(&session->ssl, &context->config);
    if (result == 0) result = mbedtls_ssl_set_hostname(&session->ssl, server_name);
    if (result != 0) {
        if (out_error) *out_error = copy_error("Mbed TLS session setup failed", result);
        mbedtls_ssl_free(&session->ssl);
        free(session);
        return MAELYS_HTTP_ERR_TLS;
    }
    mbedtls_ssl_set_bio(&session->ssl, session, socket_send, socket_receive, NULL);
    *out_session = session;
    return MAELYS_HTTP_OK;
}

static maelys_http_io_step_t handshake(void *context, void *opaque) {
    provider_session_t *session = opaque;
    (void)context;
    return translate(session, mbedtls_ssl_handshake(&session->ssl), 0);
}
static maelys_http_io_step_t tls_read(
    void *context, void *opaque, void *buffer, size_t capacity,
    size_t *out_read) {
    provider_session_t *session = opaque;
    int result;
    (void)context;
    if (out_read) *out_read = 0u;
    if (!session || !buffer || !capacity || !out_read) return MAELYS_HTTP_IO_FAILED;
    result = mbedtls_ssl_read(&session->ssl, buffer, capacity);
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
    /* TLS 1.3 post-handshake tickets carry no application bytes. Consume a
     * bounded train before exposing readiness to the HTTP state machine. */
    {
        unsigned ticket_count = 0u;
        while (result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET &&
               ticket_count++ < 8u) {
            result = mbedtls_ssl_read(&session->ssl, buffer, capacity);
        }
    }
#endif
    if (result > 0) *out_read = (size_t)result;
    return translate(session, result, 1);
}
static maelys_http_io_step_t tls_write(
    void *context, void *opaque, const void *buffer, size_t length,
    size_t *out_written) {
    provider_session_t *session = opaque;
    int result;
    (void)context;
    if (out_written) *out_written = 0u;
    if (!session || !buffer || !length || !out_written) return MAELYS_HTTP_IO_FAILED;
    result = mbedtls_ssl_write(&session->ssl, buffer, length);
    if (result > 0) *out_written = (size_t)result;
    return translate(session, result, 0);
}
static maelys_http_io_step_t shutdown_tls(void *context, void *opaque) {
    provider_session_t *session = opaque;
    (void)context;
    return translate(session, mbedtls_ssl_close_notify(&session->ssl), 0);
}
static const char *last_error(void *context, const void *opaque) {
    const provider_session_t *session = opaque;
    (void)context;
    return session && session->error[0] ? session->error : "Mbed TLS operation failed";
}
static void session_release(void *context, void *opaque) {
    provider_session_t *session = opaque;
    (void)context;
    if (!session) return;
    mbedtls_ssl_free(&session->ssl);
    free(session);
}
static void context_release(void *opaque) {
    provider_context_t *context = opaque;
    if (!context) return;
    mbedtls_ssl_config_free(&context->config);
    mbedtls_x509_crt_free(&context->authorities);
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_ctr_drbg_free(&context->random);
    mbedtls_entropy_free(&context->entropy);
#endif
    free(context);
}

maelys_http_result_t maelys_http_tls_mbedtls_client_create(
    const maelys_http_tls_client_files_t *files,
    maelys_http_tls_provider_t **out_provider, char **out_error) {
    provider_context_t *context;
    maelys_http_tls_ops_t ops;
    int result = 0;
    if (out_provider) *out_provider = NULL;
    if (out_error) *out_error = NULL;
    if (!files || !files->ca_file || !files->ca_file[0] || !out_provider) {
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
#if MBEDTLS_VERSION_MAJOR >= 4
    (void)pthread_once(&psa_once, initialize_psa);
    if (psa_status != PSA_SUCCESS) return MAELYS_HTTP_ERR_TLS;
#endif
    context = calloc(1u, sizeof(*context));
    if (!context) return MAELYS_HTTP_ERR_MEMORY;
    mbedtls_ssl_config_init(&context->config);
    mbedtls_x509_crt_init(&context->authorities);
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_entropy_init(&context->entropy);
    mbedtls_ctr_drbg_init(&context->random);
    {
        static const unsigned char personalization[] = "maelys-http";
        result = mbedtls_ctr_drbg_seed(&context->random, mbedtls_entropy_func,
            &context->entropy, personalization, sizeof(personalization) - 1u);
    }
#endif
    if (result == 0) result = mbedtls_x509_crt_parse_file(
        &context->authorities, files->ca_file);
    if (result == 0) result = mbedtls_ssl_config_defaults(
        &context->config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT);
    if (result == 0) {
        mbedtls_ssl_conf_authmode(&context->config, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&context->config, &context->authorities, NULL);
#if MBEDTLS_VERSION_MAJOR > 3 || \
    (MBEDTLS_VERSION_MAJOR == 3 && MBEDTLS_VERSION_MINOR >= 6)
        mbedtls_ssl_conf_min_tls_version(
            &context->config, MBEDTLS_SSL_VERSION_TLS1_2);
#else
        mbedtls_ssl_conf_min_version(
            &context->config, MBEDTLS_SSL_MAJOR_VERSION_3,
            MBEDTLS_SSL_MINOR_VERSION_3);
#endif
#if MBEDTLS_VERSION_MAJOR < 4
        mbedtls_ssl_conf_rng(&context->config, mbedtls_ctr_drbg_random,
                             &context->random);
#endif
    }
    if (result != 0) {
        if (out_error) *out_error = copy_error("Mbed TLS client setup failed", result);
        context_release(context);
        return MAELYS_HTTP_ERR_TLS;
    }
    memset(&ops, 0, sizeof(ops));
    ops.abi_version = MAELYS_HTTP_TLS_ABI_VERSION;
    ops.name = "mbedtls-client";
    ops.session_create = session_create;
    ops.handshake = handshake;
    ops.read = tls_read;
    ops.write = tls_write;
    ops.shutdown = shutdown_tls;
    ops.last_error = last_error;
    ops.session_release = session_release;
    {
        maelys_http_result_t create_result = maelys_http_tls_provider_create(
            &ops, context, context_release, out_provider, out_error);
        if (create_result != MAELYS_HTTP_OK) context_release(context);
        return create_result;
    }
}
