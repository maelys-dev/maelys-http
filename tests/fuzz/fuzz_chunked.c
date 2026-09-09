#include "maelys/http.h"

#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

typedef struct outcome {
    maelys_http_result_t result;
    uint64_t body_bytes;
    uint64_t body_hash;
    size_t consumed;
    size_t trailers;
} outcome_t;

static maelys_http_result_t capture(
    void *opaque, const unsigned char *bytes, size_t length) {
    outcome_t *outcome = opaque;
    size_t index;
    for (index = 0u; index < length; ++index) {
        outcome->body_hash ^= bytes[index];
        outcome->body_hash *= UINT64_C(1099511628211);
    }
    return MAELYS_HTTP_OK;
}

static outcome_t parse(
    const unsigned char *wire, size_t length, size_t fragment) {
    outcome_t outcome;
    maelys_http_parser_t *parser = NULL;
    maelys_http_result_t result = MAELYS_HTTP_AGAIN;
    size_t offset = 0u;
    memset(&outcome, 0, sizeof(outcome));
    outcome.body_hash = UINT64_C(1469598103934665603);
    if (maelys_http_parser_create(MAELYS_HTTP_PARSE_RESPONSE, NULL,
                                  capture, &outcome, &parser) != MAELYS_HTTP_OK) abort();
    while (offset < length && result == MAELYS_HTTP_AGAIN) {
        size_t amount = fragment < length - offset ? fragment : length - offset;
        size_t consumed = 0u;
        result = maelys_http_parser_feed(parser, wire + offset, amount, &consumed);
        if (consumed > amount || (!consumed && result == MAELYS_HTTP_AGAIN)) abort();
        offset += consumed;
    }
    outcome.result = result;
    outcome.body_bytes = maelys_http_parser_body_bytes(parser);
    outcome.consumed = offset;
    outcome.trailers = maelys_http_parser_trailer_count(parser);
    maelys_http_parser_release(parser);
    return outcome;
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
    static const char prefix[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    unsigned char *wire;
    outcome_t whole;
    outcome_t bytewise;
    size_t length;
    if (size > 1024u * 1024u ||
        size > SIZE_MAX - (sizeof(prefix) - 1u)) return 0;
    length = sizeof(prefix) - 1u + size;
    wire = malloc(length ? length : 1u);
    if (!wire) return 0;
    memcpy(wire, prefix, sizeof(prefix) - 1u);
    if (size) memcpy(wire + sizeof(prefix) - 1u, data, size);
    whole = parse(wire, length, length ? length : 1u);
    bytewise = parse(wire, length, 1u);
    if (whole.result != bytewise.result ||
        whole.body_bytes != bytewise.body_bytes ||
        whole.body_hash != bytewise.body_hash ||
        whole.consumed != bytewise.consumed ||
        whole.trailers != bytewise.trailers) abort();
    free(wire);
    return 0;
}
