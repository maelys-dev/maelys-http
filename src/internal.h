#ifndef MAELYS_HTTP_INTERNAL_H
#define MAELYS_HTTP_INTERNAL_H

#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#include "maelys/http.h"
#include "maelys/http_client.h"
#include "maelys/http_tls.h"

typedef struct owned_header {
    char *name;
    char *value;
} owned_header_t;

static inline int maelys_http_internal_is_tchar(unsigned char byte) {
    if ((byte >= (unsigned char)'0' && byte <= (unsigned char)'9') ||
        (byte >= (unsigned char)'A' && byte <= (unsigned char)'Z') ||
        (byte >= (unsigned char)'a' && byte <= (unsigned char)'z')) return 1;
    return strchr("!#$%&'*+-.^_`|~", (int)byte) != NULL;
}

static inline int maelys_http_internal_header_value_valid(
    const char *value, size_t length) {
    size_t index;
    if (!value && length) return 0;
    for (index = 0; index < length; ++index) {
        unsigned char byte = (unsigned char)value[index];
        if (byte == '\t') continue;
        if (byte < 0x20u || byte == 0x7fu) return 0;
    }
    return 1;
}

static inline unsigned char maelys_http_internal_ascii_lower(
    unsigned char byte) {
    return byte >= (unsigned char)'A' && byte <= (unsigned char)'Z' ?
        (unsigned char)(byte - (unsigned char)'A' + (unsigned char)'a') : byte;
}

static inline int maelys_http_internal_ascii_equal(
    const char *left, size_t left_length, const char *right) {
    size_t index;
    size_t right_length = right ? strlen(right) : 0u;
    if (!left || !right || left_length != right_length) return 0;
    for (index = 0; index < left_length; ++index) {
        if (maelys_http_internal_ascii_lower((unsigned char)left[index]) !=
            maelys_http_internal_ascii_lower((unsigned char)right[index])) return 0;
    }
    return 1;
}

static inline int maelys_http_internal_is_hex(unsigned char byte) {
    return (byte >= (unsigned char)'0' && byte <= (unsigned char)'9') ||
           (byte >= (unsigned char)'A' && byte <= (unsigned char)'F') ||
           (byte >= (unsigned char)'a' && byte <= (unsigned char)'f');
}

static inline int maelys_http_internal_ipv4_valid(
    const char *text, size_t length) {
    size_t index = 0u;
    unsigned fields = 0u;
    while (index < length) {
        unsigned value = 0u;
        size_t digits = 0u;
        if (fields == 4u) return 0;
        while (index < length && text[index] != '.') {
            unsigned digit;
            if (text[index] < '0' || text[index] > '9' || digits == 3u) return 0;
            digit = (unsigned)(text[index] - '0');
            value = value * 10u + digit;
            if (value > 255u) return 0;
            ++digits;
            ++index;
        }
        if (!digits) return 0;
        ++fields;
        if (index < length) ++index;
    }
    return fields == 4u;
}

static inline int maelys_http_internal_ip_literal_valid(
    const char *text, size_t length) {
    size_t index = 0u;
    unsigned groups = 0u;
    int compressed = 0;
    if (!text || !length) return 0;
    if (text[0] == 'v' || text[0] == 'V') {
        size_t version_start = ++index;
        while (index < length &&
               maelys_http_internal_is_hex((unsigned char)text[index])) ++index;
        if (index == version_start || index >= length || text[index++] != '.' ||
            index == length) return 0;
        for (; index < length; ++index) {
            unsigned char byte = (unsigned char)text[index];
            if (!((byte >= (unsigned char)'A' && byte <= (unsigned char)'Z') ||
                  (byte >= (unsigned char)'a' && byte <= (unsigned char)'z') ||
                  (byte >= (unsigned char)'0' && byte <= (unsigned char)'9') ||
                  strchr("-._~!$&'()*+,;=:", (int)byte))) return 0;
        }
        return 1;
    }
    if (length >= 2u && text[0] == ':' && text[1] == ':') {
        compressed = 1;
        index = 2u;
        if (index == length) return 1;
    } else if (text[0] == ':') {
        return 0;
    }
    while (index < length) {
        size_t start = index;
        while (index < length && text[index] != ':') ++index;
        if (memchr(text + start, '.', index - start)) {
            if (index != length ||
                !maelys_http_internal_ipv4_valid(text + start,
                                                  index - start)) return 0;
            groups += 2u;
            break;
        }
        if (index == start || index - start > 4u) return 0;
        while (start < index) {
            if (!maelys_http_internal_is_hex((unsigned char)text[start++])) return 0;
        }
        ++groups;
        if (groups > 8u || index == length) break;
        if (index + 1u < length && text[index + 1u] == ':') {
            if (compressed) return 0;
            compressed = 1;
            index += 2u;
            if (index == length) break;
        } else {
            ++index;
            if (index == length) return 0;
        }
    }
    return compressed ? groups < 8u : groups == 8u;
}

static inline int maelys_http_internal_uri_chars_valid(
    const char *text, size_t length, int allow_slash, int allow_query) {
    size_t index;
    for (index = 0u; index < length; ++index) {
        unsigned char byte = (unsigned char)text[index];
        if (byte == '%') {
            if (index + 2u >= length ||
                !maelys_http_internal_is_hex((unsigned char)text[index + 1u]) ||
                !maelys_http_internal_is_hex((unsigned char)text[index + 2u])) {
                return 0;
            }
            index += 2u;
            continue;
        }
        if ((byte >= (unsigned char)'A' && byte <= (unsigned char)'Z') ||
            (byte >= (unsigned char)'a' && byte <= (unsigned char)'z') ||
            (byte >= (unsigned char)'0' && byte <= (unsigned char)'9') ||
            strchr("-._~!$&'()*+,;=:@", (int)byte) ||
            (allow_slash && byte == '/') || (allow_query && byte == '?')) {
            continue;
        }
        return 0;
    }
    return 1;
}

static inline int maelys_http_internal_origin_target_valid(
    const char *target, size_t length) {
    return target && length && target[0] == '/' &&
        maelys_http_internal_uri_chars_valid(target, length, 1, 1);
}

static inline int maelys_http_internal_authority_valid(
    const char *authority, size_t length, int require_port) {
    size_t index;
    size_t colon = SIZE_MAX;
    size_t port_start = SIZE_MAX;
    unsigned port = 0u;
    if (!authority || !length) return 0;
    for (index = 0u; index < length; ++index) {
        unsigned char byte = (unsigned char)authority[index];
        if (byte <= 0x20u || byte == 0x7fu || byte == '@' || byte == '/' ||
            byte == '?' || byte == '#' || byte == ',') return 0;
    }
    if (authority[0] == '[') {
        const char *closing = memchr(authority + 1u, ']', length - 1u);
        size_t closing_index;
        if (!closing || closing == authority + 1u) return 0;
        closing_index = (size_t)(closing - authority);
        if (!maelys_http_internal_ip_literal_valid(authority + 1u,
                                                    closing_index - 1u)) return 0;
        if (closing_index + 1u == length) return !require_port;
        if (closing_index + 2u >= length || closing[1] != ':') return 0;
        port_start = closing_index + 2u;
    } else {
        for (index = 0u; index < length; ++index) {
            if (authority[index] == ':') {
                if (colon != SIZE_MAX) return 0;
                colon = index;
            }
        }
        if (!maelys_http_internal_uri_chars_valid(
                authority, colon == SIZE_MAX ? length : colon, 0, 0)) return 0;
        if (colon == SIZE_MAX) return !require_port;
        if (colon == 0u || colon + 1u == length) return 0;
        port_start = colon + 1u;
    }
    for (index = port_start; index < length; ++index) {
        unsigned digit;
        if (authority[index] < '0' || authority[index] > '9') return 0;
        digit = (unsigned)(authority[index] - '0');
        if (port > (65535u - digit) / 10u) return 0;
        port = port * 10u + digit;
    }
    return port > 0u;
}

static inline int maelys_http_internal_method_target_valid(
    const char *method, size_t method_length,
    const char *target, size_t target_length) {
    size_t index;
    int is_connect = method_length == 7u && !memcmp(method, "CONNECT", 7u);
    int is_options = method_length == 7u && !memcmp(method, "OPTIONS", 7u);
    if (!method || !target || !target_length) return 0;
    for (index = 0u; index < target_length; ++index) {
        unsigned char byte = (unsigned char)target[index];
        if (byte <= 0x20u || byte == 0x7fu || byte == '#') return 0;
    }
    if (target_length == 1u && target[0] == '*') return is_options;
    if (is_connect) {
        return maelys_http_internal_authority_valid(
            target, target_length, 1);
    }
    if (target[0] == '/') {
        return maelys_http_internal_origin_target_valid(target, target_length);
    }
    for (index = 1u; index + 2u < target_length; ++index) {
        if (target[index] == ':' && target[index + 1u] == '/' &&
            target[index + 2u] == '/') {
            size_t scheme_index;
            size_t authority_start = index + 3u;
            size_t authority_end = authority_start;
            if (!((target[0] >= 'A' && target[0] <= 'Z') ||
                  (target[0] >= 'a' && target[0] <= 'z'))) return 0;
            for (scheme_index = 1u; scheme_index < index; ++scheme_index) {
                unsigned char byte = (unsigned char)target[scheme_index];
                if (!((byte >= 'A' && byte <= 'Z') ||
                      (byte >= 'a' && byte <= 'z') ||
                      (byte >= '0' && byte <= '9') || byte == '+' ||
                      byte == '-' || byte == '.')) return 0;
            }
            while (authority_end < target_length &&
                   target[authority_end] != '/' &&
                   target[authority_end] != '?') ++authority_end;
            if (!maelys_http_internal_authority_valid(
                    target + authority_start,
                    authority_end - authority_start, 0)) return 0;
            if (authority_end == target_length) return 1;
            if (target[authority_end] != '/' && target[authority_end] != '?') {
                return 0;
            }
            return maelys_http_internal_uri_chars_valid(
                target + authority_end, target_length - authority_end, 1, 1);
        }
    }
    return 0;
}

static inline char *maelys_http_internal_strndup(
    const char *value, size_t length) {
    char *copy;
    if ((!value && length) || length == SIZE_MAX) return NULL;
    copy = malloc(length + 1u);
    if (!copy) return NULL;
    if (length) memcpy(copy, value, length);
    copy[length] = '\0';
    return copy;
}

static inline char *maelys_http_internal_strdup(const char *value) {
    return value ? maelys_http_internal_strndup(value, strlen(value)) : NULL;
}

static inline void maelys_http_internal_set_error(
    char **destination, const char *message) {
    if (!destination) return;
    free(*destination);
    *destination = maelys_http_internal_strdup(
        message ? message : "unknown error");
}

static inline void maelys_http_internal_free_headers(
    owned_header_t *headers, size_t count) {
    size_t index;
    if (!headers) return;
    for (index = 0; index < count; ++index) {
        free(headers[index].name);
        free(headers[index].value);
    }
    free(headers);
}

struct maelys_http_tls_provider {
    atomic_uint references;
    maelys_http_tls_ops_t ops;
    char *name;
    void *context;
    void (*release_context)(void *context);
};

struct maelys_http_tls_session {
    maelys_http_tls_provider_t *provider;
    void *implementation;
};

struct maelys_http_transport {
    atomic_uint references;
    maelys_http_transport_ops_t ops;
    char *name;
    void *context;
    void (*release_context)(void *context);
};

#endif
