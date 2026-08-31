#ifndef MAELYS_HTTP_TLS_MODULES_H
#define MAELYS_HTTP_TLS_MODULES_H

#include "maelys/http_tls.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct maelys_http_tls_client_files {
    const char *ca_file;
} maelys_http_tls_client_files_t;

/* Optional module, linked separately from the core/client archives. */
maelys_http_result_t maelys_http_tls_mbedtls_client_create(
    const maelys_http_tls_client_files_t *files,
    maelys_http_tls_provider_t **out_provider,
    char **out_error);

#ifdef __cplusplus
}
#endif

#endif
