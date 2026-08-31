#include "internal.h"

const char *maelys_http_result_string(maelys_http_result_t result) {
    switch (result) {
        case MAELYS_HTTP_OK: return "ok";
        case MAELYS_HTTP_AGAIN: return "again";
        case MAELYS_HTTP_COMPLETE: return "complete";
        case MAELYS_HTTP_ERR_ARGUMENT: return "invalid argument";
        case MAELYS_HTTP_ERR_MEMORY: return "out of memory";
        case MAELYS_HTTP_ERR_LIMIT: return "limit exceeded";
        case MAELYS_HTTP_ERR_SYNTAX: return "invalid HTTP syntax";
        case MAELYS_HTTP_ERR_FRAMING: return "ambiguous HTTP framing";
        case MAELYS_HTTP_ERR_STATE: return "invalid state";
        case MAELYS_HTTP_ERR_IO: return "I/O failure";
        case MAELYS_HTTP_ERR_TIMEOUT: return "deadline expired";
        case MAELYS_HTTP_ERR_CANCELLED: return "cancelled";
        case MAELYS_HTTP_ERR_TLS: return "TLS failure";
    }
    return "unknown result";
}

void maelys_http_limits_default(maelys_http_limits_t *limits) {
    if (!limits) return;
    limits->max_start_line_bytes = 8192u;
    limits->max_header_line_bytes = 8192u;
    limits->max_header_bytes = 65536u;
    limits->max_header_count = 128u;
    limits->max_body_bytes = UINT64_C(64) * 1024u * 1024u;
    limits->max_chunk_line_bytes = 1024u;
    limits->max_trailer_bytes = 16384u;
    limits->max_trailer_count = 32u;
}
