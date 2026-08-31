#include "maelys/http.h"

#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

typedef struct outcome {
    maelys_http_result_t result;
    maelys_http_body_framing_t framing;
    uint64_t body_bytes;
    uint64_t content_length;
    size_t consumed;
    size_t headers;
    size_t trailers;
    unsigned status;
    uint64_t body_hash;
} outcome_t;

static uint64_t hash_bytes(uint64_t hash, const unsigned char *bytes, size_t length) {
    size_t index;
    for (index = 0u; index < length; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static maelys_http_result_t capture(
    void *context, const unsigned char *bytes, size_t length) {
    outcome_t *outcome = context;
    outcome->body_hash = hash_bytes(outcome->body_hash, bytes, length);
    return MAELYS_HTTP_OK;
}

static outcome_t parse(
    const unsigned char *data, size_t size, size_t fragment) {
    outcome_t outcome;
    maelys_http_parser_t *parser = NULL;
    maelys_http_result_t result = MAELYS_HTTP_AGAIN;
    size_t offset = 0u;
    memset(&outcome, 0, sizeof(outcome));
    outcome.result = MAELYS_HTTP_AGAIN;
    outcome.body_hash = UINT64_C(1469598103934665603);
    if (maelys_http_parser_create(MAELYS_HTTP_PARSE_RESPONSE, NULL,
                                  capture, &outcome, &parser) != MAELYS_HTTP_OK) abort();
    while (offset < size && result == MAELYS_HTTP_AGAIN) {
        size_t amount = fragment < size - offset ? fragment : size - offset;
        size_t consumed = 0u;
        result = maelys_http_parser_feed(parser, data + offset, amount, &consumed);
        if (consumed > amount || (!consumed && result == MAELYS_HTTP_AGAIN)) abort();
        offset += consumed;
    }
    outcome.result = result;
    outcome.framing = maelys_http_parser_body_framing(parser);
    outcome.body_bytes = maelys_http_parser_body_bytes(parser);
    outcome.content_length = maelys_http_parser_content_length(parser);
    outcome.consumed = offset;
    outcome.headers = maelys_http_parser_header_count(parser);
    outcome.trailers = maelys_http_parser_trailer_count(parser);
    outcome.status = maelys_http_parser_status(parser);
    maelys_http_parser_release(parser);
    return outcome;
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
    outcome_t whole = parse(data, size, size ? size : 1u);
    outcome_t bytewise = parse(data, size, 1u);
    if (whole.result != bytewise.result || whole.framing != bytewise.framing ||
        whole.body_bytes != bytewise.body_bytes ||
        whole.content_length != bytewise.content_length ||
        whole.consumed != bytewise.consumed ||
        whole.headers != bytewise.headers || whole.trailers != bytewise.trailers ||
        whole.status != bytewise.status || whole.body_hash != bytewise.body_hash) abort();
    return 0;
}
