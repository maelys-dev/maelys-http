#ifndef MAELYS_HTTP_TRANSPORTS_H
#define MAELYS_HTTP_TRANSPORTS_H

#include "maelys/http_client.h"
#include "maelys/http_tls.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * POSIX connector for HTTP and HTTPS. Socket lifecycle, connect, partial I/O
 * and shutdown use the opaque maelys-system 0.5 socket API. Address order,
 * retry and TLS remain HTTP transport policy. The default resolver is the
 * explicitly compatibility-only bounded getaddrinfo provider: exchange
 * cancellation is memory-safe but cannot interrupt the OS/NSS call.
 */
maelys_http_result_t maelys_http_posix_transport_create(
    maelys_http_tls_provider_t *tls_provider,
    maelys_http_transport_t **out_transport,
    char **out_error);

#ifdef __cplusplus
}
#endif

#endif
