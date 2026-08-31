#include "maelys/http_tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); return 1; \
} } while (0)

typedef struct fake_context { char server_name[128]; size_t releases; } fake_context_t;
typedef struct fake_session { char error[64]; } fake_session_t;

static maelys_http_result_t create_session(
    void *opaque, int fd, const char *server_name, void **out_session,
    char **out_error) {
    fake_context_t *context = opaque;
    fake_session_t *session;
    (void)fd; (void)out_error;
    session = calloc(1u, sizeof(*session));
    if (!session) return MAELYS_HTTP_ERR_MEMORY;
    (void)snprintf(context->server_name, sizeof(context->server_name), "%s",
                   server_name);
    *out_session = session;
    return MAELYS_HTTP_OK;
}
static maelys_http_io_step_t handshake(void *context, void *session) {
    (void)context; (void)session; return MAELYS_HTTP_IO_COMPLETE;
}
static maelys_http_io_step_t read_tls(
    void *context, void *session, void *buffer, size_t capacity, size_t *out) {
    (void)context; (void)session; (void)buffer; (void)capacity; *out = 0u;
    return MAELYS_HTTP_IO_WANT_READ;
}
static maelys_http_io_step_t write_tls(
    void *context, void *session, const void *buffer, size_t length, size_t *out) {
    (void)context; (void)session; (void)buffer; *out = length;
    return MAELYS_HTTP_IO_COMPLETE;
}
static maelys_http_io_step_t shutdown_tls(void *context, void *session) {
    (void)context; (void)session; return MAELYS_HTTP_IO_COMPLETE;
}
static const char *last_error(void *context, const void *session) {
    (void)context; (void)session; return "fake error";
}
static void release_session(void *opaque, void *session) {
    fake_context_t *context = opaque;
    ++context->releases;
    free(session);
}

int main(void) {
    fake_context_t context = {{0}, 0u};
    maelys_http_tls_ops_t ops;
    maelys_http_tls_provider_t *provider = NULL;
    maelys_http_tls_session_t *session = NULL;
    char *error = NULL;
    size_t amount = 0u;
    memset(&ops, 0, sizeof(ops));
    ops.abi_version = MAELYS_HTTP_TLS_ABI_VERSION;
    ops.name = "fake";
    ops.session_create = create_session;
    ops.handshake = handshake;
    ops.read = read_tls;
    ops.write = write_tls;
    ops.shutdown = shutdown_tls;
    ops.last_error = last_error;
    ops.session_release = release_session;
    CHECK(maelys_http_tls_provider_create(&ops, &context, NULL,
                                          &provider, &error) == MAELYS_HTTP_OK);
    CHECK(provider != NULL && error == NULL);
    CHECK(maelys_http_tls_session_create_client(provider, 4, "api.example",
                                                &session, &error) == MAELYS_HTTP_OK);
    CHECK(!strcmp(context.server_name, "api.example"));
    CHECK(maelys_http_tls_session_handshake(session) == MAELYS_HTTP_IO_COMPLETE);
    CHECK(maelys_http_tls_session_read(session, context.server_name,
                                       sizeof(context.server_name), &amount) ==
          MAELYS_HTTP_IO_WANT_READ);
    CHECK(maelys_http_tls_session_write(session, "x", 1u, &amount) ==
          MAELYS_HTTP_IO_COMPLETE && amount == 1u);
    maelys_http_tls_session_release(session);
    CHECK(context.releases == 1u);
    maelys_http_tls_provider_release(provider);

    ops.abi_version = 999u;
    provider = NULL;
    CHECK(maelys_http_tls_provider_create(&ops, &context, NULL,
                                          &provider, &error) == MAELYS_HTTP_ERR_ARGUMENT);
    CHECK(provider == NULL && error != NULL);
    free(error);
    puts("test_tls_provider: ok");
    return 0;
}
