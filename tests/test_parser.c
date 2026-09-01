#include "maelys/http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

typedef struct body_capture {
    unsigned char bytes[4096];
    size_t length;
} body_capture_t;

static maelys_http_result_t capture_body(
    void *opaque, const unsigned char *bytes, size_t length) {
    body_capture_t *capture = opaque;
    if (capture->length + length > sizeof(capture->bytes)) return MAELYS_HTTP_ERR_LIMIT;
    memcpy(capture->bytes + capture->length, bytes, length);
    capture->length += length;
    return MAELYS_HTTP_OK;
}

static maelys_http_result_t invalid_success_body(
    void *opaque, const unsigned char *bytes, size_t length) {
    (void)opaque; (void)bytes; (void)length;
    return MAELYS_HTTP_COMPLETE;
}

static int parse_fragmented(
    maelys_http_parser_kind_t kind, const unsigned char *wire, size_t length,
    size_t fragment, maelys_http_result_t expected, body_capture_t *capture,
    maelys_http_parser_t **out_parser) {
    maelys_http_parser_t *parser = NULL;
    maelys_http_result_t result;
    size_t offset = 0u;
    CHECK(maelys_http_parser_create(kind, NULL, capture_body, capture,
                                    &parser) == MAELYS_HTTP_OK);
    result = MAELYS_HTTP_AGAIN;
    while (offset < length && result == MAELYS_HTTP_AGAIN) {
        size_t amount = fragment < length - offset ? fragment : length - offset;
        size_t consumed = 0u;
        result = maelys_http_parser_feed(parser, wire + offset, amount, &consumed);
        CHECK(consumed <= amount);
        offset += consumed;
        if (!consumed && result == MAELYS_HTTP_AGAIN) break;
    }
    CHECK(result == expected);
    if (out_parser) *out_parser = parser;
    else maelys_http_parser_release(parser);
    return 0;
}

static int parse_at_every_cut(
    maelys_http_parser_kind_t kind, const unsigned char *wire, size_t length) {
    size_t cut;
    for (cut = 1u; cut < length; ++cut) {
        body_capture_t capture = {{0}, 0u};
        maelys_http_parser_t *parser = NULL;
        size_t consumed = 0u;
        CHECK(maelys_http_parser_create(kind, NULL, capture_body, &capture,
                                        &parser) == MAELYS_HTTP_OK);
        CHECK(maelys_http_parser_feed(parser, wire, cut, &consumed) ==
              MAELYS_HTTP_AGAIN);
        CHECK(consumed == cut);
        consumed = 0u;
        CHECK(maelys_http_parser_feed(parser, wire + cut, length - cut,
                                      &consumed) == MAELYS_HTTP_COMPLETE);
        CHECK(consumed == length - cut);
        maelys_http_parser_release(parser);
    }
    return 0;
}

static int test_request_all_fragment_sizes(void) {
    static const unsigned char wire[] =
        "POST /v1/items HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Content-Length: 5\r\n"
        "X-Test: yes\r\n\r\nhello";
    size_t fragment;
    for (fragment = 1u; fragment <= sizeof(wire) - 1u; ++fragment) {
        body_capture_t capture = {{0}, 0u};
        maelys_http_parser_t *parser = NULL;
        CHECK(parse_fragmented(MAELYS_HTTP_PARSE_REQUEST, wire,
            sizeof(wire) - 1u, fragment, MAELYS_HTTP_COMPLETE,
            &capture, &parser) == 0);
        CHECK(maelys_http_parser_method(parser).length == 4u);
        CHECK(!memcmp(maelys_http_parser_method(parser).data, "POST", 4u));
        CHECK(maelys_http_parser_target(parser).length == 9u);
        CHECK(maelys_http_parser_header_count(parser) == 3u);
        CHECK(maelys_http_parser_body_framing(parser) ==
              MAELYS_HTTP_BODY_CONTENT_LENGTH);
        CHECK(capture.length == 5u && !memcmp(capture.bytes, "hello", 5u));
        maelys_http_parser_release(parser);
    }
    CHECK(parse_at_every_cut(MAELYS_HTTP_PARSE_REQUEST, wire,
                             sizeof(wire) - 1u) == 0);
    return 0;
}

static int test_chunked_response_and_trailers(void) {
    static const unsigned char wire[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4;safe=yes\r\nWiki\r\n5\r\npedia\r\n0\r\nX-Digest: ok\r\n\r\n";
    size_t fragment;
    for (fragment = 1u; fragment <= 17u; ++fragment) {
        body_capture_t capture = {{0}, 0u};
        maelys_http_parser_t *parser = NULL;
        maelys_http_header_view_t trailer;
        CHECK(parse_fragmented(MAELYS_HTTP_PARSE_RESPONSE, wire,
            sizeof(wire) - 1u, fragment, MAELYS_HTTP_COMPLETE,
            &capture, &parser) == 0);
        CHECK(maelys_http_parser_status(parser) == 200u);
        CHECK(capture.length == 9u && !memcmp(capture.bytes, "Wikipedia", 9u));
        CHECK(maelys_http_parser_trailer_count(parser) == 1u);
        trailer = maelys_http_parser_trailer(parser, 0u);
        CHECK(trailer.name.length == 8u && !memcmp(trailer.name.data, "X-Digest", 8u));
        maelys_http_parser_release(parser);
    }
    CHECK(parse_at_every_cut(MAELYS_HTTP_PARSE_RESPONSE, wire,
                             sizeof(wire) - 1u) == 0);
    return 0;
}

static int test_close_delimited_and_head(void) {
    static const unsigned char wire[] = "HTTP/1.1 200 OK\r\nX: y\r\n\r\nabc";
    body_capture_t capture = {{0}, 0u};
    maelys_http_parser_t *parser = NULL;
    size_t consumed = 0u;
    CHECK(maelys_http_parser_create(MAELYS_HTTP_PARSE_RESPONSE, NULL,
                                    capture_body, &capture, &parser) == MAELYS_HTTP_OK);
    CHECK(maelys_http_parser_feed(parser, wire, sizeof(wire) - 1u,
                                  &consumed) == MAELYS_HTTP_AGAIN);
    CHECK(consumed == sizeof(wire) - 1u && capture.length == 3u);
    CHECK(maelys_http_parser_eof(parser) == MAELYS_HTTP_COMPLETE);
    maelys_http_parser_release(parser);

    capture.length = 0u;
    parser = NULL;
    CHECK(maelys_http_parser_create(MAELYS_HTTP_PARSE_RESPONSE, NULL,
                                    capture_body, &capture, &parser) == MAELYS_HTTP_OK);
    CHECK(maelys_http_parser_set_response_to_head(parser, 1) == MAELYS_HTTP_OK);
    consumed = 0u;
    CHECK(maelys_http_parser_feed(parser, wire, sizeof(wire) - 1u,
                                  &consumed) == MAELYS_HTTP_COMPLETE);
    CHECK(capture.length == 0u && consumed < sizeof(wire) - 1u);
    maelys_http_parser_release(parser);
    return 0;
}

static int test_responses_without_message_body(void) {
    static const char *wire[] = {
        "HTTP/1.1 100 Continue\r\nX: y\r\n\r\nignored",
        "HTTP/1.1 204 No Content\r\nX: y\r\n\r\nignored",
        ("HTTP/1.1 304 Not Modified\r\n"
         "Content-Length: 18446744073709551615\r\n\r\nignored")
    };
    size_t index;
    for (index = 0u; index < sizeof(wire) / sizeof(wire[0]); ++index) {
        body_capture_t capture = {{0}, 0u};
        maelys_http_parser_t *parser = NULL;
        size_t consumed = 0u;
        CHECK(maelys_http_parser_create(MAELYS_HTTP_PARSE_RESPONSE, NULL,
                                        capture_body, &capture,
                                        &parser) == MAELYS_HTTP_OK);
        CHECK(maelys_http_parser_feed(parser, wire[index], strlen(wire[index]),
                                      &consumed) == MAELYS_HTTP_COMPLETE);
        CHECK(capture.length == 0u);
        CHECK(consumed < strlen(wire[index]));
        CHECK(maelys_http_parser_body_framing(parser) == MAELYS_HTTP_BODY_NONE);
        maelys_http_parser_release(parser);
    }
    {
        static const char connect_wire[] =
            "HTTP/1.1 200 Connection Established\r\nX: y\r\n\r\ntunnel";
        body_capture_t capture = {{0}, 0u};
        maelys_http_parser_t *parser = NULL;
        size_t consumed = 0u;
        CHECK(maelys_http_parser_create(MAELYS_HTTP_PARSE_RESPONSE, NULL,
                                        capture_body, &capture,
                                        &parser) == MAELYS_HTTP_OK);
        CHECK(maelys_http_parser_set_response_to_connect(parser, 1) ==
              MAELYS_HTTP_OK);
        CHECK(maelys_http_parser_feed(parser, connect_wire,
                                      sizeof(connect_wire) - 1u,
                                      &consumed) == MAELYS_HTTP_COMPLETE);
        CHECK(capture.length == 0u && consumed < sizeof(connect_wire) - 1u);
        CHECK(!memcmp(connect_wire + consumed, "tunnel", 6u));
        maelys_http_parser_release(parser);
    }
    {
        static const char invalid_status[] = "HTTP/1.1 200\r\n\r\n";
        static const char out_of_range[] = "HTTP/1.1 600 Invalid\r\n\r\n";
        maelys_http_parser_t *parser = NULL;
        size_t consumed = 0u;
        CHECK(maelys_http_parser_create(MAELYS_HTTP_PARSE_RESPONSE, NULL,
                                        NULL, NULL, &parser) == MAELYS_HTTP_OK);
        CHECK(maelys_http_parser_feed(parser, invalid_status,
                                      sizeof(invalid_status) - 1u,
                                      &consumed) == MAELYS_HTTP_ERR_SYNTAX);
        maelys_http_parser_release(parser);
        parser = NULL;
        consumed = 0u;
        CHECK(maelys_http_parser_create(MAELYS_HTTP_PARSE_RESPONSE, NULL,
                                        NULL, NULL, &parser) == MAELYS_HTTP_OK);
        CHECK(maelys_http_parser_feed(parser, out_of_range,
                                      sizeof(out_of_range) - 1u,
                                      &consumed) == MAELYS_HTTP_ERR_SYNTAX);
        maelys_http_parser_release(parser);
    }
    {
        static const char obs_fold_response[] =
            "HTTP/1.1 200 OK\r\nX: y\r\n folded\r\n\r\n";
        maelys_http_parser_t *parser = NULL;
        size_t consumed = 0u;
        CHECK(maelys_http_parser_create(MAELYS_HTTP_PARSE_RESPONSE, NULL,
                                        NULL, NULL, &parser) == MAELYS_HTTP_OK);
        CHECK(maelys_http_parser_feed(parser, obs_fold_response,
                                      sizeof(obs_fold_response) - 1u,
                                      &consumed) == MAELYS_HTTP_ERR_SYNTAX);
        maelys_http_parser_release(parser);
    }
    return 0;
}

static int expect_reject(const unsigned char *wire, size_t length,
                         maelys_http_result_t expected) {
    body_capture_t capture = {{0}, 0u};
    maelys_http_parser_t *parser = NULL;
    size_t consumed = 0u;
    maelys_http_result_t result;
    CHECK(maelys_http_parser_create(MAELYS_HTTP_PARSE_REQUEST, NULL,
                                    capture_body, &capture, &parser) == MAELYS_HTTP_OK);
    result = maelys_http_parser_feed(parser, wire, length, &consumed);
    CHECK(result == expected);
    consumed = 0u;
    CHECK(maelys_http_parser_feed(parser, "GET / HTTP/1.1\r\n\r\n", 18u,
                                  &consumed) == expected);
    CHECK(consumed == 0u);
    maelys_http_parser_release(parser);
    return 0;
}

static int test_adversarial(void) {
    static const unsigned char missing_host[] =
        "GET / HTTP/1.1\r\nX: y\r\n\r\n";
    static const unsigned char duplicate_host[] =
        "GET / HTTP/1.1\r\nHost: one\r\nHost: two\r\n\r\n";
    static const unsigned char empty_host[] =
        "GET / HTTP/1.1\r\nHost: \r\n\r\n";
    static const unsigned char spaced_host[] =
        "GET / HTTP/1.1\r\nHost: a b\r\n\r\n";
    static const unsigned char comma_host[] =
        "GET / HTTP/1.1\r\nHost: a,b\r\n\r\n";
    static const unsigned char userinfo_host[] =
        "GET / HTTP/1.1\r\nHost: user@host\r\n\r\n";
    static const unsigned char bare_lf[] = "GET / HTTP/1.1\nHost: x\n\n";
    static const unsigned char obs_fold[] =
        "GET / HTTP/1.1\r\nHost: x\r\n folded\r\n\r\n";
    static const unsigned char duplicate_cl[] =
        "POST / HTTP/1.1\r\nHost: example.test\r\n"
        "Content-Length: 1\r\nContent-Length: 1\r\n\r\nx";
    static const unsigned char cl_te[] =
        "POST / HTTP/1.1\r\nHost: example.test\r\n"
        "Content-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n";
    static const unsigned char bad_te[] =
        "POST / HTTP/1.1\r\nHost: example.test\r\n"
        "Transfer-Encoding: gzip, chunked\r\n\r\n";
    static const unsigned char overflow[] =
        "POST / HTTP/1.1\r\nHost: example.test\r\n"
        "Content-Length: 18446744073709551616\r\n\r\n";
    static const unsigned char nul[] =
        "GET / HTTP/1.1\r\nX: a\0b\r\n\r\n";
    static const unsigned char bad_trailer[] =
        "POST / HTTP/1.1\r\nHost: example.test\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "0\r\nContent-Length: 0\r\n\r\n";
    static const unsigned char non_ascii_name[] =
        "GET / HTTP/1.1\r\nX-\xe9: value\r\n\r\n";
    static const unsigned char fragment_target[] =
        "GET /path#fragment HTTP/1.1\r\n\r\n";
    static const unsigned char bad_percent_target[] =
        "GET /a%ZZ HTTP/1.1\r\nHost: example.test\r\n\r\n";
    static const unsigned char backslash_target[] =
        "GET /a\\b HTTP/1.1\r\nHost: example.test\r\n\r\n";
    CHECK(expect_reject(bare_lf, sizeof(bare_lf) - 1u, MAELYS_HTTP_ERR_SYNTAX) == 0);
    CHECK(expect_reject(missing_host, sizeof(missing_host) - 1u,
                        MAELYS_HTTP_ERR_FRAMING) == 0);
    CHECK(expect_reject(duplicate_host, sizeof(duplicate_host) - 1u,
                        MAELYS_HTTP_ERR_FRAMING) == 0);
    CHECK(expect_reject(empty_host, sizeof(empty_host) - 1u,
                        MAELYS_HTTP_ERR_FRAMING) == 0);
    CHECK(expect_reject(spaced_host, sizeof(spaced_host) - 1u,
                        MAELYS_HTTP_ERR_FRAMING) == 0);
    CHECK(expect_reject(comma_host, sizeof(comma_host) - 1u,
                        MAELYS_HTTP_ERR_FRAMING) == 0);
    CHECK(expect_reject(userinfo_host, sizeof(userinfo_host) - 1u,
                        MAELYS_HTTP_ERR_FRAMING) == 0);
    CHECK(expect_reject(obs_fold, sizeof(obs_fold) - 1u, MAELYS_HTTP_ERR_SYNTAX) == 0);
    CHECK(expect_reject(duplicate_cl, sizeof(duplicate_cl) - 1u,
                        MAELYS_HTTP_ERR_FRAMING) == 0);
    CHECK(expect_reject(cl_te, sizeof(cl_te) - 1u, MAELYS_HTTP_ERR_FRAMING) == 0);
    CHECK(expect_reject(bad_te, sizeof(bad_te) - 1u, MAELYS_HTTP_ERR_FRAMING) == 0);
    CHECK(expect_reject(overflow, sizeof(overflow) - 1u, MAELYS_HTTP_ERR_FRAMING) == 0);
    CHECK(expect_reject(nul, sizeof(nul) - 1u, MAELYS_HTTP_ERR_SYNTAX) == 0);
    CHECK(expect_reject(bad_trailer, sizeof(bad_trailer) - 1u,
                        MAELYS_HTTP_ERR_FRAMING) == 0);
    CHECK(expect_reject(non_ascii_name, sizeof(non_ascii_name) - 1u,
                        MAELYS_HTTP_ERR_SYNTAX) == 0);
    CHECK(expect_reject(fragment_target, sizeof(fragment_target) - 1u,
                        MAELYS_HTTP_ERR_SYNTAX) == 0);
    CHECK(expect_reject(bad_percent_target, sizeof(bad_percent_target) - 1u,
                        MAELYS_HTTP_ERR_SYNTAX) == 0);
    CHECK(expect_reject(backslash_target, sizeof(backslash_target) - 1u,
                        MAELYS_HTTP_ERR_SYNTAX) == 0);
    return 0;
}

static int test_chunk_extension_grammar(void) {
    static const char *invalid[] = {
        "1; \r\na\r\n0\r\n\r\n",
        "1;=x\r\na\r\n0\r\n\r\n",
        "1;name=\"unterminated\r\na\r\n0\r\n\r\n",
        "1;name=has space\r\na\r\n0\r\n\r\n"
    };
    static const char valid[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "1; name = \"a\\\"b\";token=value\r\na\r\n0\r\n\r\n";
    static const char prefix[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    size_t index;
    body_capture_t capture = {{0}, 0u};
    maelys_http_parser_t *parser = NULL;
    CHECK(parse_fragmented(MAELYS_HTTP_PARSE_RESPONSE,
        (const unsigned char *)valid, sizeof(valid) - 1u, 1u,
        MAELYS_HTTP_COMPLETE, &capture, NULL) == 0);
    for (index = 0u; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        char wire[256];
        size_t consumed = 0u;
        maelys_http_result_t result;
        (void)snprintf(wire, sizeof(wire), "%s%s", prefix, invalid[index]);
        CHECK(maelys_http_parser_create(MAELYS_HTTP_PARSE_RESPONSE, NULL,
                                        NULL, NULL, &parser) == MAELYS_HTTP_OK);
        result = maelys_http_parser_feed(parser, wire, strlen(wire), &consumed);
        CHECK(result == MAELYS_HTTP_ERR_SYNTAX);
        maelys_http_parser_release(parser);
        parser = NULL;
    }
    return 0;
}

static int test_body_callback_contract(void) {
    static const char wire[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx";
    maelys_http_parser_t *parser = NULL;
    size_t consumed = 0u;
    CHECK(maelys_http_parser_create(MAELYS_HTTP_PARSE_RESPONSE, NULL,
                                    invalid_success_body, NULL,
                                    &parser) == MAELYS_HTTP_OK);
    CHECK(maelys_http_parser_feed(parser, wire, sizeof(wire) - 1u,
                                  &consumed) == MAELYS_HTTP_ERR_STATE);
    CHECK(maelys_http_parser_result(parser) == MAELYS_HTTP_ERR_STATE);
    maelys_http_parser_release(parser);
    return 0;
}

static int test_limits(void) {
    static const char header_wire[] =
        "GET / HTTP/1.1\r\nX:                 \r\n\r\n";
    static const char trailer_wire[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "0\r\nX:             \r\n\r\n";
    maelys_http_limits_t limits;
    maelys_http_parser_t *parser = NULL;
    size_t consumed = 0u;
    maelys_http_limits_default(&limits);
    limits.max_header_count = 1u;
    CHECK(maelys_http_parser_create(MAELYS_HTTP_PARSE_REQUEST, &limits,
                                    NULL, NULL, &parser) == MAELYS_HTTP_OK);
    CHECK(maelys_http_parser_feed(parser,
        "GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\n\r\n", 35u, &consumed) ==
        MAELYS_HTTP_ERR_LIMIT);
    maelys_http_parser_release(parser);

    maelys_http_limits_default(&limits);
    limits.max_header_bytes = 16u;
    consumed = 0u;
    CHECK(maelys_http_parser_create(MAELYS_HTTP_PARSE_REQUEST, &limits,
                                    NULL, NULL, &parser) == MAELYS_HTTP_OK);
    CHECK(maelys_http_parser_feed(parser, header_wire,
        sizeof(header_wire) - 1u, &consumed) ==
        MAELYS_HTTP_ERR_LIMIT);
    maelys_http_parser_release(parser);

    maelys_http_limits_default(&limits);
    limits.max_trailer_bytes = 12u;
    consumed = 0u;
    CHECK(maelys_http_parser_create(MAELYS_HTTP_PARSE_RESPONSE, &limits,
                                    NULL, NULL, &parser) == MAELYS_HTTP_OK);
    CHECK(maelys_http_parser_feed(parser, trailer_wire,
        sizeof(trailer_wire) - 1u, &consumed) ==
        MAELYS_HTTP_ERR_LIMIT);
    maelys_http_parser_release(parser);

    /* Line limits count exact wire bytes, including both CR and LF. */
    maelys_http_limits_default(&limits);
    limits.max_start_line_bytes = 17u;
    limits.max_header_line_bytes = 9u;
    limits.max_header_bytes = 11u;
    consumed = 0u;
    CHECK(maelys_http_parser_create(MAELYS_HTTP_PARSE_REQUEST, &limits,
                                    NULL, NULL, &parser) == MAELYS_HTTP_OK);
    CHECK(maelys_http_parser_feed(parser,
        "GET /x HTTP/1.1\r\nHost: x\r\n\r\n", 28u, &consumed) ==
        MAELYS_HTTP_COMPLETE);
    maelys_http_parser_release(parser);

    maelys_http_limits_default(&limits);
    limits.max_start_line_bytes = 17u;
    consumed = 0u;
    CHECK(maelys_http_parser_create(MAELYS_HTTP_PARSE_REQUEST, &limits,
                                    NULL, NULL, &parser) == MAELYS_HTTP_OK);
    CHECK(maelys_http_parser_feed(parser,
        "GET /xy HTTP/1.1\r\nHost: x\r\n\r\n", 29u, &consumed) ==
        MAELYS_HTTP_ERR_LIMIT);
    maelys_http_parser_release(parser);

    maelys_http_limits_default(&limits);
    limits.max_header_line_bytes = 9u;
    consumed = 0u;
    CHECK(maelys_http_parser_create(MAELYS_HTTP_PARSE_REQUEST, &limits,
                                    NULL, NULL, &parser) == MAELYS_HTTP_OK);
    CHECK(maelys_http_parser_feed(parser,
        "GET / HTTP/1.1\r\nHost: xx\r\n\r\n", 28u, &consumed) ==
        MAELYS_HTTP_ERR_LIMIT);
    maelys_http_parser_release(parser);
    return 0;
}

int main(void) {
    CHECK(test_request_all_fragment_sizes() == 0);
    CHECK(test_chunked_response_and_trailers() == 0);
    CHECK(test_close_delimited_and_head() == 0);
    CHECK(test_responses_without_message_body() == 0);
    CHECK(test_adversarial() == 0);
    CHECK(test_chunk_extension_grammar() == 0);
    CHECK(test_body_callback_contract() == 0);
    CHECK(test_limits() == 0);
    puts("test_parser: ok");
    return 0;
}
