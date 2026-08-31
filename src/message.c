#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct maelys_http_message {
    int response;
    char *method;
    char *target;
    unsigned status;
    char *reason;
    owned_header_t *headers;
    size_t header_count;
    int framing_set;
    int chunked;
    uint64_t content_length;
};

static int token_string(const char *value) {
    size_t index;
    if (!value || !value[0]) return 0;
    for (index = 0; value[index]; ++index) {
        if (!maelys_http_internal_is_tchar((unsigned char)value[index])) return 0;
    }
    return 1;
}

static maelys_http_result_t create_common(
    int response, maelys_http_message_t **out_message) {
    maelys_http_message_t *message;
    if (out_message) *out_message = NULL;
    if (!out_message) return MAELYS_HTTP_ERR_ARGUMENT;
    message = calloc(1u, sizeof(*message));
    if (!message) return MAELYS_HTTP_ERR_MEMORY;
    message->response = response;
    *out_message = message;
    return MAELYS_HTTP_OK;
}

maelys_http_result_t maelys_http_request_create(
    const char *method, const char *target, maelys_http_message_t **out_message) {
    maelys_http_message_t *message;
    maelys_http_result_t result;
    if (!token_string(method) || !target ||
        !maelys_http_internal_method_target_valid(
            method, strlen(method), target, strlen(target))) {
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    result = create_common(0, &message);
    if (result != MAELYS_HTTP_OK) return result;
    message->method = maelys_http_internal_strdup(method);
    message->target = maelys_http_internal_strdup(target);
    if (!message->method || !message->target) {
        maelys_http_message_release(message);
        return MAELYS_HTTP_ERR_MEMORY;
    }
    *out_message = message;
    return MAELYS_HTTP_OK;
}

maelys_http_result_t maelys_http_response_create(
    unsigned status, const char *reason, maelys_http_message_t **out_message) {
    maelys_http_message_t *message;
    maelys_http_result_t result;
    if (status < 100u || status > 599u || !reason ||
        !maelys_http_internal_header_value_valid(reason, strlen(reason))) {
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    result = create_common(1, &message);
    if (result != MAELYS_HTTP_OK) return result;
    message->status = status;
    message->reason = maelys_http_internal_strdup(reason);
    if (!message->reason) {
        maelys_http_message_release(message);
        return MAELYS_HTTP_ERR_MEMORY;
    }
    *out_message = message;
    return MAELYS_HTTP_OK;
}

maelys_http_result_t maelys_http_message_add_header(
    maelys_http_message_t *message, const char *name, const char *value) {
    owned_header_t *headers;
    size_t index;
    if (!message || !name || !value || !name[0] ||
        !maelys_http_internal_header_value_valid(value, strlen(value))) {
        return MAELYS_HTTP_ERR_ARGUMENT;
    }
    for (index = 0; name[index]; ++index) {
        if (!maelys_http_internal_is_tchar((unsigned char)name[index])) return MAELYS_HTTP_ERR_ARGUMENT;
    }
    if (maelys_http_internal_ascii_equal(name, strlen(name), "content-length") ||
        maelys_http_internal_ascii_equal(name, strlen(name), "transfer-encoding")) {
        return MAELYS_HTTP_ERR_FRAMING;
    }
    if (!message->response &&
        maelys_http_internal_ascii_equal(name, strlen(name), "host")) {
        if (!maelys_http_internal_authority_valid(
                value, strlen(value), 0)) return MAELYS_HTTP_ERR_FRAMING;
        for (index = 0u; index < message->header_count; ++index) {
            if (maelys_http_internal_ascii_equal(
                    message->headers[index].name,
                    strlen(message->headers[index].name), "host")) {
                return MAELYS_HTTP_ERR_FRAMING;
            }
        }
    }
    if (message->header_count >= 1024u) return MAELYS_HTTP_ERR_LIMIT;
    headers = realloc(message->headers,
                      (message->header_count + 1u) * sizeof(*headers));
    if (!headers) return MAELYS_HTTP_ERR_MEMORY;
    message->headers = headers;
    headers[message->header_count].name = maelys_http_internal_strdup(name);
    headers[message->header_count].value = maelys_http_internal_strdup(value);
    if (!headers[message->header_count].name ||
        !headers[message->header_count].value) {
        free(headers[message->header_count].name);
        free(headers[message->header_count].value);
        headers[message->header_count].name = NULL;
        headers[message->header_count].value = NULL;
        return MAELYS_HTTP_ERR_MEMORY;
    }
    ++message->header_count;
    return MAELYS_HTTP_OK;
}

maelys_http_result_t maelys_http_message_set_content_length(
    maelys_http_message_t *message, uint64_t length) {
    if (!message || message->framing_set) return MAELYS_HTTP_ERR_STATE;
    message->framing_set = 1;
    message->content_length = length;
    return MAELYS_HTTP_OK;
}

maelys_http_result_t maelys_http_message_set_chunked(maelys_http_message_t *message) {
    if (!message || message->framing_set) return MAELYS_HTTP_ERR_STATE;
    message->framing_set = 1;
    message->chunked = 1;
    return MAELYS_HTTP_OK;
}

static maelys_http_result_t sink_all(
    maelys_http_sink_fn sink, void *context, const void *bytes, size_t length) {
    const unsigned char *cursor = bytes;
    size_t offset = 0u;
    if (!sink || (!bytes && length)) return MAELYS_HTTP_ERR_ARGUMENT;
    while (offset < length) {
        size_t written = 0u;
        maelys_http_result_t result = sink(context, cursor + offset,
                                           length - offset, &written);
        if (result == MAELYS_HTTP_AGAIN || result == MAELYS_HTTP_COMPLETE) {
            return MAELYS_HTTP_ERR_STATE;
        }
        if (result != MAELYS_HTTP_OK) return result;
        if (!written || written > length - offset) return MAELYS_HTTP_ERR_IO;
        offset += written;
    }
    return MAELYS_HTTP_OK;
}

maelys_http_result_t maelys_http_message_write_head(
    const maelys_http_message_t *message, maelys_http_sink_fn sink,
    void *sink_context) {
    char line[128];
    int count;
    size_t index;
    maelys_http_result_t result;
    if (!message || !sink) return MAELYS_HTTP_ERR_ARGUMENT;
    if (!message->response) {
        size_t host_count = 0u;
        for (index = 0u; index < message->header_count; ++index) {
            if (maelys_http_internal_ascii_equal(
                    message->headers[index].name,
                    strlen(message->headers[index].name), "host") &&
                message->headers[index].value[0]) ++host_count;
        }
        if (host_count != 1u) return MAELYS_HTTP_ERR_FRAMING;
    }
    if (message->response) {
        count = snprintf(line, sizeof(line), "HTTP/1.1 %03u ", message->status);
        if (count < 0 || (size_t)count >= sizeof(line)) return MAELYS_HTTP_ERR_LIMIT;
        result = sink_all(sink, sink_context, line, (size_t)count);
        if (result == MAELYS_HTTP_OK) result = sink_all(
            sink, sink_context, message->reason, strlen(message->reason));
        if (result == MAELYS_HTTP_OK) result = sink_all(
            sink, sink_context, "\r\n", 2u);
    } else {
        result = sink_all(sink, sink_context, message->method,
                          strlen(message->method));
        if (result == MAELYS_HTTP_OK) result = sink_all(
            sink, sink_context, " ", 1u);
        if (result == MAELYS_HTTP_OK) result = sink_all(
            sink, sink_context, message->target, strlen(message->target));
        if (result == MAELYS_HTTP_OK) result = sink_all(
            sink, sink_context, " HTTP/1.1\r\n", 11u);
    }
    if (result != MAELYS_HTTP_OK) return result;
    for (index = 0; index < message->header_count; ++index) {
        owned_header_t *header = &message->headers[index];
        result = sink_all(sink, sink_context, header->name, strlen(header->name));
        if (result == MAELYS_HTTP_OK) result = sink_all(sink, sink_context, ": ", 2u);
        if (result == MAELYS_HTTP_OK) result = sink_all(sink, sink_context,
                                                       header->value,
                                                       strlen(header->value));
        if (result == MAELYS_HTTP_OK) result = sink_all(sink, sink_context, "\r\n", 2u);
        if (result != MAELYS_HTTP_OK) return result;
    }
    if (message->framing_set) {
        if (message->chunked) {
            result = sink_all(sink, sink_context,
                              "Transfer-Encoding: chunked\r\n", 28u);
        } else {
            count = snprintf(line, sizeof(line), "Content-Length: %llu\r\n",
                             (unsigned long long)message->content_length);
            if (count < 0 || (size_t)count >= sizeof(line)) return MAELYS_HTTP_ERR_LIMIT;
            result = sink_all(sink, sink_context, line, (size_t)count);
        }
        if (result != MAELYS_HTTP_OK) return result;
    }
    return sink_all(sink, sink_context, "\r\n", 2u);
}

void maelys_http_message_release(maelys_http_message_t *message) {
    if (!message) return;
    free(message->method);
    free(message->target);
    free(message->reason);
    maelys_http_internal_free_headers(message->headers, message->header_count);
    free(message);
}

maelys_http_result_t maelys_http_write_chunk(
    const void *bytes, size_t length, maelys_http_sink_fn sink,
    void *sink_context) {
    char line[32];
    int count;
    maelys_http_result_t result;
    if ((!bytes && length) || !sink || !length) return MAELYS_HTTP_ERR_ARGUMENT;
    count = snprintf(line, sizeof(line), "%zx\r\n", length);
    if (count < 0 || (size_t)count >= sizeof(line)) return MAELYS_HTTP_ERR_LIMIT;
    result = sink_all(sink, sink_context, line, (size_t)count);
    if (result == MAELYS_HTTP_OK) result = sink_all(sink, sink_context, bytes, length);
    if (result == MAELYS_HTTP_OK) result = sink_all(sink, sink_context, "\r\n", 2u);
    return result;
}

maelys_http_result_t maelys_http_write_chunk_end(
    const maelys_http_header_view_t *trailers, size_t trailer_count,
    maelys_http_sink_fn sink, void *sink_context) {
    size_t index;
    maelys_http_result_t result;
    if ((!trailers && trailer_count) || !sink) return MAELYS_HTTP_ERR_ARGUMENT;
    for (index = 0; index < trailer_count; ++index) {
        const maelys_http_header_view_t *header = &trailers[index];
        size_t byte;
        if (!header->name.length || !header->name.data ||
            (!header->value.data && header->value.length) ||
            !maelys_http_internal_header_value_valid(header->value.data, header->value.length) ||
            maelys_http_internal_ascii_equal(header->name.data, header->name.length, "content-length") ||
            maelys_http_internal_ascii_equal(header->name.data, header->name.length, "transfer-encoding") ||
            maelys_http_internal_ascii_equal(header->name.data, header->name.length, "trailer")) {
            return MAELYS_HTTP_ERR_ARGUMENT;
        }
        for (byte = 0; byte < header->name.length; ++byte) {
            if (!maelys_http_internal_is_tchar((unsigned char)header->name.data[byte])) {
                return MAELYS_HTTP_ERR_ARGUMENT;
            }
        }
    }
    result = sink_all(sink, sink_context, "0\r\n", 3u);
    for (index = 0; result == MAELYS_HTTP_OK && index < trailer_count; ++index) {
        const maelys_http_header_view_t *header = &trailers[index];
        result = sink_all(sink, sink_context, header->name.data, header->name.length);
        if (result == MAELYS_HTTP_OK) result = sink_all(sink, sink_context, ": ", 2u);
        if (result == MAELYS_HTTP_OK) result = sink_all(sink, sink_context,
                                                       header->value.data,
                                                       header->value.length);
        if (result == MAELYS_HTTP_OK) result = sink_all(sink, sink_context, "\r\n", 2u);
    }
    if (result == MAELYS_HTTP_OK) result = sink_all(sink, sink_context, "\r\n", 2u);
    return result;
}
