#include "maelys/http_client.h"
#include "maelys/http_tls_modules.h"
#include "maelys/http_transports.h"
#include "maelys/sys/clock.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    maelys_http_tls_client_files_t files;
    maelys_http_tls_provider_t *tls = NULL;
    maelys_http_transport_t *transport = NULL;
    maelys_http_client_t *client = NULL;
    maelys_http_request_t *request = NULL;
    maelys_http_exchange_t *exchange = NULL;
    char *error = NULL;
    uint64_t deadline = 0u;
    maelys_http_result_t result;
    if (argc != 3) return 64;
    files.ca_file = argv[1];
    result = maelys_http_tls_mbedtls_client_create(&files, &tls, &error);
    if (result == MAELYS_HTTP_OK) {
        result = maelys_http_posix_transport_create(tls, &transport, &error);
    }
    if (result == MAELYS_HTTP_OK) result = maelys_http_client_create(
        transport, NULL, &client);
    if (result == MAELYS_HTTP_OK) result = maelys_http_request_config_create(
        "GET", "https", argv[2], "/", &request);
    if (result == MAELYS_HTTP_OK &&
        maelys_sys_deadline_after(5000u, &deadline) != MAELYS_SYS_OK) {
        result = MAELYS_HTTP_ERR_IO;
    }
    if (result == MAELYS_HTTP_OK) result = maelys_http_exchange_create(
        client, request, deadline, &exchange);
    if (result == MAELYS_HTTP_OK) {
        size_t advances = 0u;
        do {
            result = maelys_http_exchange_advance(exchange);
            ++advances;
        } while (result == MAELYS_HTTP_AGAIN && advances < 10000u);
        if (result == MAELYS_HTTP_AGAIN) result = MAELYS_HTTP_ERR_TIMEOUT;
    }
    if (result != MAELYS_HTTP_COMPLETE || maelys_http_exchange_status(exchange) != 200u) {
        fprintf(stderr, "%s\n", error ? error :
            (exchange && maelys_http_exchange_error(exchange) ?
             maelys_http_exchange_error(exchange) : maelys_http_result_string(result)));
        result = result == MAELYS_HTTP_COMPLETE ? MAELYS_HTTP_ERR_IO : result;
    }
    free(error);
    maelys_http_exchange_release(exchange);
    maelys_http_request_release(request);
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    maelys_http_tls_provider_release(tls);
    return result == MAELYS_HTTP_COMPLETE ? 0 : 1;
}
