#include "maelys/http_tls_modules.h"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    maelys_http_tls_provider_t *provider = NULL;
    maelys_http_tls_client_files_t files = {"/definitely/missing/ca.pem"};
    char *error = NULL;
    maelys_http_result_t result = maelys_http_tls_mbedtls_client_create(
        &files, &provider, &error);
    if (result != MAELYS_HTTP_ERR_TLS || provider != NULL) {
        fprintf(stderr, "Mbed TLS provider accepted a missing trust anchor\n");
        return 1;
    }
    free(error);
    puts("test_tls_mbedtls: ok");
    return 0;
}
