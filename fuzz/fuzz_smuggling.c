#include "maelys/http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

static maelys_http_result_t parse_fragmented(
    const unsigned char *wire, size_t length, size_t fragment) {
    maelys_http_parser_t *parser = NULL;
    maelys_http_result_t result = MAELYS_HTTP_AGAIN;
    size_t offset = 0u;
    if (maelys_http_parser_create(MAELYS_HTTP_PARSE_REQUEST, NULL,
                                  NULL, NULL, &parser) != MAELYS_HTTP_OK) abort();
    while (offset < length && result == MAELYS_HTTP_AGAIN) {
        size_t amount = fragment < length - offset ? fragment : length - offset;
        size_t consumed = 0u;
        result = maelys_http_parser_feed(parser, wire + offset, amount, &consumed);
        if (consumed > amount || (!consumed && result == MAELYS_HTTP_AGAIN)) abort();
        offset += consumed;
    }
    if (result == MAELYS_HTTP_ERR_FRAMING || result == MAELYS_HTTP_ERR_SYNTAX ||
        result == MAELYS_HTTP_ERR_LIMIT) {
        size_t consumed = 0u;
        if (maelys_http_parser_feed(parser, "GET / HTTP/1.1\r\n\r\n", 18u,
                                    &consumed) != result || consumed != 0u) abort();
    }
    maelys_http_parser_release(parser);
    return result;
}

static void require_framing_rejection(const char *wire) {
    size_t length = strlen(wire);
    if (parse_fragmented((const unsigned char *)wire, length,
                         length ? length : 1u) != MAELYS_HTTP_ERR_FRAMING ||
        parse_fragmented((const unsigned char *)wire, length, 1u) !=
            MAELYS_HTTP_ERR_FRAMING) abort();
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
    char cl_te[256];
    char duplicate[256];
    unsigned left = size ? (unsigned)data[0] : 0u;
    unsigned right = size > 1u ? (unsigned)data[1] : left;
    (void)snprintf(cl_te, sizeof(cl_te),
        "POST / HTTP/1.1\r\nHost: fuzz.test\r\nContent-Length: %u\r\n"
        "Transfer-Encoding: chunked\r\n\r\n", left);
    (void)snprintf(duplicate, sizeof(duplicate),
        "POST / HTTP/1.1\r\nHost: fuzz.test\r\nContent-Length: %u\r\n"
        "Content-Length: %u\r\n\r\n", left, right);
    require_framing_rejection(cl_te);
    require_framing_rejection(duplicate);
    (void)parse_fragmented(data, size, size ? size : 1u);
    return 0;
}
