#include "maelys/http_client.h"
#include "maelys/http_tls_modules.h"
#include "maelys/http_transports.h"
#include "maelys/sys/clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct capture {
    unsigned char bytes[16];
    size_t length;
} capture_t;

static maelys_http_sink_step_t capture_body(
    void *opaque, const unsigned char *bytes, size_t length) {
    capture_t *capture = opaque;
    if (capture->length + length > sizeof(capture->bytes)) {
        return MAELYS_HTTP_SINK_FAILED;
    }
    memcpy(capture->bytes + capture->length, bytes, length);
    capture->length += length;
    return MAELYS_HTTP_SINK_ACCEPT;
}

static maelys_http_result_t perform_get(
    maelys_http_client_t *client, const char *authority, const char *target,
    const char *expected_body, char **out_diagnostic) {
    maelys_http_request_t *request = NULL;
    maelys_http_exchange_t *exchange = NULL;
    capture_t capture = {{0}, 0u};
    uint64_t deadline = 0u;
    maelys_http_result_t result;
    size_t advances = 0u;
    if (out_diagnostic) *out_diagnostic = NULL;
    result = maelys_http_request_config_create(
        "GET", "https", authority, target, &request);
    if (result == MAELYS_HTTP_OK) {
        result = maelys_http_request_set_response_sink(
            request, capture_body, &capture);
    }
    if (result == MAELYS_HTTP_OK &&
        maelys_sys_deadline_after(5000u, &deadline) != MAELYS_SYS_OK) {
        result = MAELYS_HTTP_ERR_IO;
    }
    if (result == MAELYS_HTTP_OK) {
        result = maelys_http_exchange_create(
            client, request, deadline, &exchange);
    }
    while (result == MAELYS_HTTP_OK || result == MAELYS_HTTP_AGAIN) {
        result = maelys_http_exchange_advance(exchange);
        if (++advances >= 10000u && result == MAELYS_HTTP_AGAIN) {
            result = MAELYS_HTTP_ERR_TIMEOUT;
        }
    }
    if (result == MAELYS_HTTP_COMPLETE &&
        (maelys_http_exchange_status(exchange) != 200u ||
         capture.length != strlen(expected_body) ||
         memcmp(capture.bytes, expected_body, capture.length))) {
        result = MAELYS_HTTP_ERR_IO;
    }
    if (result != MAELYS_HTTP_COMPLETE && out_diagnostic) {
        const char *message = exchange ? maelys_http_exchange_error(exchange) : NULL;
        *out_diagnostic = strdup(message ? message : maelys_http_result_string(result));
    }
    maelys_http_exchange_release(exchange);
    maelys_http_request_release(request);
    return result;
}

int main(int argc, char **argv) {
    maelys_http_tls_client_files_t files;
    maelys_http_tls_provider_t *tls = NULL;
    maelys_http_transport_t *transport = NULL;
    maelys_http_client_t *client = NULL;
    const char *mode;
    char *error = NULL;
    maelys_http_result_t result;
    if (argc != 3 && argc != 4) return 64;
    mode = argc == 4 ? argv[3] : "single";
    if (strcmp(mode, "single") && strcmp(mode, "reuse") &&
        strcmp(mode, "close") && strcmp(mode, "silent-close")) return 64;
    files.ca_file = argv[1];
    result = maelys_http_tls_mbedtls_client_create(&files, &tls, &error);
    if (result == MAELYS_HTTP_OK) {
        result = maelys_http_posix_transport_create(tls, &transport, &error);
    }
    if (result == MAELYS_HTTP_OK) result = maelys_http_client_create(
        transport, NULL, &client);
    if (result == MAELYS_HTTP_OK && strcmp(mode, "single")) {
        result = maelys_http_client_set_connection_reuse(client, 1);
    }
    if (result == MAELYS_HTTP_OK) {
        const char *first_target = "/";
        const char *first_body = "ok";
        if (!strcmp(mode, "reuse")) {
            first_target = "/one";
            first_body = "one";
        } else if (!strcmp(mode, "close")) {
            first_target = "/close";
            first_body = "close";
        } else if (!strcmp(mode, "silent-close")) {
            first_target = "/silent-close";
            first_body = "silent";
        }
        result = perform_get(client, argv[2], first_target, first_body, &error);
    }
    if (result == MAELYS_HTTP_COMPLETE && !strcmp(mode, "silent-close")) {
        const struct timespec settle = {0, 100000000L};
        (void)nanosleep(&settle, NULL);
    }
    if (result == MAELYS_HTTP_COMPLETE && strcmp(mode, "single")) {
        result = perform_get(client, argv[2], "/two", "two", &error);
    }
    if (result != MAELYS_HTTP_COMPLETE) {
        fprintf(stderr, "%s\n", error ? error : maelys_http_result_string(result));
    }
    free(error);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    maelys_http_tls_provider_release(tls);
    return result == MAELYS_HTTP_COMPLETE ? 0 : 1;
}
