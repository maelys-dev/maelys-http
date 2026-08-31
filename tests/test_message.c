#include "maelys/http.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); return 1; \
} } while (0)

typedef struct output { char bytes[4096]; size_t length; size_t maximum; } output_t;

static maelys_http_result_t write_output(
    void *opaque, const void *bytes, size_t length, size_t *out_written) {
    output_t *output = opaque;
    size_t amount = length < output->maximum ? length : output->maximum;
    if (output->length + amount > sizeof(output->bytes)) return MAELYS_HTTP_ERR_LIMIT;
    memcpy(output->bytes + output->length, bytes, amount);
    output->length += amount;
    *out_written = amount;
    return MAELYS_HTTP_OK;
}

static maelys_http_result_t pause_output(
    void *opaque, const void *bytes, size_t length, size_t *out_written) {
    (void)opaque; (void)bytes; (void)length;
    *out_written = 0u;
    return MAELYS_HTTP_AGAIN;
}

int main(void) {
    maelys_http_message_t *request = NULL;
    maelys_http_message_t *response = NULL;
    output_t output = {{0}, 0u, 3u};
    CHECK(maelys_http_request_create("POST", "/mcp", &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_request_create("GET", "/bad%ZZ", &response) ==
          MAELYS_HTTP_ERR_ARGUMENT);
    CHECK(maelys_http_response_create(600u, "Invalid", &response) ==
          MAELYS_HTTP_ERR_ARGUMENT);
    CHECK(maelys_http_message_add_header(request, "Host", "example.test") ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_message_add_header(request, "Host", "duplicate.test") ==
          MAELYS_HTTP_ERR_FRAMING);
    CHECK(maelys_http_message_add_header(request, "Content-Length", "5") ==
          MAELYS_HTTP_ERR_FRAMING);
    CHECK(maelys_http_message_set_chunked(request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_message_write_head(request, write_output, &output) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_write_chunk("hello", 5u, write_output, &output) ==
          MAELYS_HTTP_OK);
    CHECK(maelys_http_write_chunk_end(NULL, 0u, write_output, &output) ==
          MAELYS_HTTP_OK);
    CHECK(output.length == strlen("POST /mcp HTTP/1.1\r\nHost: example.test\r\n"
                                  "Transfer-Encoding: chunked\r\n\r\n"
                                  "5\r\nhello\r\n0\r\n\r\n"));
    CHECK(!memcmp(output.bytes,
        "POST /mcp HTTP/1.1\r\nHost: example.test\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n0\r\n\r\n", output.length));
    {
        maelys_http_header_view_t invalid = {{"Bad Name", 8u}, {"x", 1u}};
        size_t before = output.length;
        CHECK(maelys_http_write_chunk_end(&invalid, 1u, write_output, &output) ==
              MAELYS_HTTP_ERR_ARGUMENT);
        CHECK(output.length == before);
    }
    CHECK(maelys_http_write_chunk("x", 1u, pause_output, NULL) ==
          MAELYS_HTTP_ERR_STATE);
    maelys_http_message_release(request);
    request = NULL;
    output.length = 0u;
    CHECK(maelys_http_request_create("GET", "/", &request) == MAELYS_HTTP_OK);
    CHECK(maelys_http_message_write_head(request, write_output, &output) ==
          MAELYS_HTTP_ERR_FRAMING);
    CHECK(maelys_http_message_add_header(request, "Host", "") ==
          MAELYS_HTTP_ERR_FRAMING);
    CHECK(maelys_http_message_add_header(request, "Host", "a b") ==
          MAELYS_HTTP_ERR_FRAMING);
    CHECK(maelys_http_message_add_header(request, "Host", "a,b") ==
          MAELYS_HTTP_ERR_FRAMING);
    CHECK(maelys_http_message_add_header(request, "Host", "user@host") ==
          MAELYS_HTTP_ERR_FRAMING);
    maelys_http_message_release(request);
    puts("test_message: ok");
    return 0;
}
