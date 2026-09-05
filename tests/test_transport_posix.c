#include "maelys/http_transports.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "maelys/sys/clock.h"
#include "maelys/sys/fd.h"

#define EXCHANGE_COUNT 12u
#define CANCEL_COUNT 48u
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    goto cleanup; \
} } while (0)

typedef struct loopback_server {
    int listener;
    size_t expected;
    atomic_int failed;
} loopback_server_t;

typedef struct capture {
    unsigned char bytes[2];
    size_t length;
} capture_t;

static int send_all(int fd, const void *bytes, size_t length) {
    const unsigned char *cursor = bytes;
    while (length) {
        size_t written = 0u;
        if (maelys_sys_socket_send_nosigpipe(
                fd, cursor, length, &written) != MAELYS_SYS_OK || !written) {
            return 0;
        }
        cursor += written;
        length -= written;
    }
    return 1;
}

static void *serve_loopback(void *opaque) {
    static const char response[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
    loopback_server_t *server = opaque;
    size_t served;
    for (served = 0u; served < server->expected; ++served) {
        char request[4096];
        size_t length = 0u;
        int client = accept(server->listener, NULL, NULL);
        if (client < 0) {
            atomic_store_explicit(&server->failed, 1, memory_order_release);
            return NULL;
        }
        while (length + 1u < sizeof(request)) {
            ssize_t received = recv(
                client, request + length, sizeof(request) - length - 1u, 0);
            if (received <= 0) break;
            length += (size_t)received;
            request[length] = '\0';
            if (strstr(request, "\r\n\r\n")) break;
        }
        if (!length || !strstr(request, "Host: localhost:") ||
            !send_all(client, response, sizeof(response) - 1u)) {
            atomic_store_explicit(&server->failed, 1, memory_order_release);
            (void)close(client);
            return NULL;
        }
        (void)close(client);
    }
    return NULL;
}

/* Serves the head of a close-delimited response, then resets the connection
 * (SO_LINGER 0) instead of closing it. The client must fail the exchange:
 * a reset is not the end of stream that completes such a body. */
static void *serve_reset(void *opaque) {
    static const char head[] = "HTTP/1.1 200 OK\r\n\r\npartial";
    loopback_server_t *server = opaque;
    struct linger abort_close;
    char request[4096];
    size_t length = 0u;
    int client = accept(server->listener, NULL, NULL);
    if (client < 0) {
        atomic_store_explicit(&server->failed, 1, memory_order_release);
        return NULL;
    }
    while (length + 1u < sizeof(request)) {
        ssize_t received = recv(
            client, request + length, sizeof(request) - length - 1u, 0);
        if (received <= 0) break;
        length += (size_t)received;
        request[length] = '\0';
        if (strstr(request, "\r\n\r\n")) break;
    }
    abort_close.l_onoff = 1;
    abort_close.l_linger = 0;
    if (!length || !send_all(client, head, sizeof(head) - 1u) ||
        setsockopt(client, SOL_SOCKET, SO_LINGER, &abort_close,
                   sizeof(abort_close)) != 0) {
        atomic_store_explicit(&server->failed, 1, memory_order_release);
    }
    (void)close(client);
    return NULL;
}

static int listen_loopback(loopback_server_t *server, char *authority,
                           size_t authority_size) {
    struct sockaddr_in address;
    socklen_t address_length = sizeof(address);
    server->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listener < 0) return 0;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
        bind(server->listener, (struct sockaddr *)&address,
             sizeof(address)) != 0 ||
        listen(server->listener, 64) != 0 ||
        getsockname(server->listener, (struct sockaddr *)&address,
                    &address_length) != 0) {
        return 0;
    }
    return snprintf(authority, authority_size, "localhost:%u",
                    (unsigned)ntohs(address.sin_port)) > 0;
}

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

static int submit_then_cancel(
    maelys_http_client_t *client, const char *authority, uint64_t deadline) {
    maelys_http_request_t *request = NULL;
    maelys_http_exchange_t *exchange = NULL;
    maelys_http_result_t result;
    if (maelys_http_request_config_create(
            "GET", "http", authority, "/cancel", &request) != MAELYS_HTTP_OK) {
        return 0;
    }
    if (maelys_http_exchange_create(
            client, request, deadline, &exchange) != MAELYS_HTTP_OK) {
        maelys_http_request_release(request);
        return 0;
    }
    result = maelys_http_exchange_advance(exchange);
    if (result != MAELYS_HTTP_AGAIN) {
        maelys_http_exchange_release(exchange);
        maelys_http_request_release(request);
        return 0;
    }
    maelys_http_exchange_cancel(exchange);
    result = maelys_http_exchange_advance(exchange);
    maelys_http_exchange_release(exchange);
    maelys_http_request_release(request);
    return result == MAELYS_HTTP_ERR_CANCELLED;
}

int main(void) {
    loopback_server_t server;
    loopback_server_t reset_server;
    pthread_t server_thread;
    pthread_t reset_thread;
    maelys_http_request_t *reset_request = NULL;
    maelys_http_exchange_t *reset_exchange = NULL;
    maelys_http_result_t reset_result = MAELYS_HTTP_AGAIN;
    char reset_authority[64];
    maelys_http_transport_t *transport = NULL;
    maelys_http_client_t *client = NULL;
    maelys_http_client_limits_t limits;
    maelys_http_exchange_t *exchanges[EXCHANGE_COUNT] = {NULL};
    capture_t captures[EXCHANGE_COUNT] = {{{0}, 0u}};
    int complete[EXCHANGE_COUNT] = {0};
    char authority[64];
    uint64_t deadline = 0u;
    size_t index;
    size_t remaining = EXCHANGE_COUNT;
    size_t rounds;
    int thread_started = 0;
    int reset_started = 0;
    int status = 1;

    memset(&server, 0, sizeof(server));
    memset(&reset_server, 0, sizeof(reset_server));
    server.listener = -1;
    reset_server.listener = -1;
    atomic_init(&server.failed, 0);
    atomic_init(&reset_server.failed, 0);
    server.expected = EXCHANGE_COUNT;
    CHECK(listen_loopback(&server, authority, sizeof(authority)));
    CHECK(pthread_create(&server_thread, NULL, serve_loopback, &server) == 0);
    thread_started = 1;
    CHECK(maelys_http_posix_transport_create(
              NULL, &transport, NULL) == MAELYS_HTTP_OK);
    maelys_http_client_limits_default(&limits);
    limits.max_progress_steps_per_advance = 1u;
    limits.max_wait_slice_ms = 10u;
    CHECK(maelys_http_client_create(transport, &limits, &client) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_sys_deadline_after(15000u, &deadline) == MAELYS_SYS_OK);

    /* Owner cancellation races resolver completion, then releases its ref.
     * The detached worker must safely own and release the final reference. */
    for (index = 0u; index < CANCEL_COUNT; ++index) {
        CHECK(submit_then_cancel(client, authority, deadline));
    }

    /* All first advances submit DNS before any exchange is progressed again,
     * so the global pool and queue are exercised concurrently. */
    for (index = 0u; index < EXCHANGE_COUNT; ++index) {
        maelys_http_request_t *request = NULL;
        CHECK(maelys_http_request_config_create(
                  "GET", "http", authority, "/", &request) == MAELYS_HTTP_OK);
        CHECK(maelys_http_request_set_response_sink(
                  request, capture_body, &captures[index]) == MAELYS_HTTP_OK);
        CHECK(maelys_http_exchange_create(
                  client, request, deadline, &exchanges[index]) == MAELYS_HTTP_OK);
        maelys_http_request_release(request);
        CHECK(maelys_http_exchange_advance(exchanges[index]) == MAELYS_HTTP_AGAIN);
    }
    for (rounds = 0u; remaining && rounds < 20000u; ++rounds) {
        for (index = 0u; index < EXCHANGE_COUNT; ++index) {
            maelys_http_result_t result;
            if (complete[index]) continue;
            result = maelys_http_exchange_advance(exchanges[index]);
            if (result == MAELYS_HTTP_COMPLETE) {
                complete[index] = 1;
                --remaining;
            } else {
                CHECK(result == MAELYS_HTTP_AGAIN);
            }
        }
    }
    CHECK(remaining == 0u);
    for (index = 0u; index < EXCHANGE_COUNT; ++index) {
        CHECK(captures[index].length == 2u &&
              !memcmp(captures[index].bytes, "ok", 2u));
    }
    CHECK(pthread_join(server_thread, NULL) == 0);
    thread_started = 0;
    CHECK(!atomic_load_explicit(&server.failed, memory_order_acquire));

    /* A close-delimited body cut by a TCP reset must fail, never complete. */
    CHECK(listen_loopback(&reset_server, reset_authority,
                          sizeof(reset_authority)));
    CHECK(pthread_create(&reset_thread, NULL, serve_reset, &reset_server) == 0);
    reset_started = 1;
    CHECK(maelys_http_request_config_create(
              "GET", "http", reset_authority, "/", &reset_request) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_exchange_create(
              client, reset_request, deadline, &reset_exchange) ==
          MAELYS_HTTP_OK);
    for (rounds = 0u; rounds < 20000u; ++rounds) {
        reset_result = maelys_http_exchange_advance(reset_exchange);
        if (reset_result != MAELYS_HTTP_AGAIN) break;
    }
    CHECK(reset_result == MAELYS_HTTP_ERR_IO);
    CHECK(maelys_http_exchange_error(reset_exchange) != NULL &&
          strstr(maelys_http_exchange_error(reset_exchange), "reset") != NULL);
    CHECK(pthread_join(reset_thread, NULL) == 0);
    reset_started = 0;
    CHECK(!atomic_load_explicit(&reset_server.failed, memory_order_acquire));
    status = 0;

cleanup:
    if (reset_exchange && reset_result == MAELYS_HTTP_AGAIN) {
        maelys_http_exchange_cancel(reset_exchange);
    }
    maelys_http_exchange_release(reset_exchange);
    maelys_http_request_release(reset_request);
    for (index = 0u; index < EXCHANGE_COUNT; ++index) {
        if (exchanges[index] && !complete[index]) {
            maelys_http_exchange_cancel(exchanges[index]);
        }
        maelys_http_exchange_release(exchanges[index]);
    }
    maelys_http_client_release(client);
    maelys_http_transport_release(transport);
    if (server.listener >= 0) {
        (void)shutdown(server.listener, SHUT_RDWR);
        (void)close(server.listener);
    }
    if (reset_server.listener >= 0) {
        (void)shutdown(reset_server.listener, SHUT_RDWR);
        (void)close(reset_server.listener);
    }
    if (thread_started) (void)pthread_join(server_thread, NULL);
    if (reset_started) (void)pthread_join(reset_thread, NULL);
    if (status) return status;
    puts("test_transport_posix: ok");
    return 0;
}
