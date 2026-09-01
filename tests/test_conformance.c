/*
 * The RESPONSE parser's adversarial wire-conformance corpus, run.
 *
 * tests/test_parser.c asserts behaviour from cases spelled as C literals.
 * This binary asserts the same parser against a corpus that lives on the
 * WIRE: every vector in conformance/response-wire-cases.txt is the exact
 * byte sequence a hostile server would put on the socket, kept as escaped
 * text (see conformance/format.md) so no literal CR, LF or NUL appears in
 * the file, and decoded here byte-for-byte.
 *
 * Every vector is asserted at several chunk fragmentations - whole, then
 * fixed steps of 1, 2, 3, 7 and 13 bytes - and each fragmentation must
 * produce the identical verdict, framing, body byte count and unconsumed
 * excess. Split-across-chunks equivalence is the class of bug an
 * incremental parser exists to have, and the class this corpus hunts.
 *
 * The corpus path is argv[1] so the Makefile names the corpus rather than
 * this binary hardcoding it. The loader is itself a bounded parser: fixed
 * buffers, a named refusal on every overflow, no allocation whose size the
 * corpus file chooses.
 */
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

#define MAX_CASES 256u
#define MAX_WIRE_BYTES 4096u
#define MAX_LINE_BYTES 8192u
#define MAX_NAME_BYTES 256u

typedef enum expect_kind {
    EXPECT_ACCEPT,
    EXPECT_REJECT,
    EXPECT_INCOMPLETE
} expect_kind_t;

typedef struct wire_case {
    char name[MAX_NAME_BYTES];
    unsigned char wire[MAX_WIRE_BYTES];
    size_t length;
    expect_kind_t kind;
    /* Meaningful only when kind is EXPECT_REJECT. */
    maelys_http_result_t reject;
    /* Meaningful only when kind is EXPECT_ACCEPT. */
    maelys_http_body_framing_t framing;
    uint64_t content_length;
    int check_body;
    uint64_t body_bytes;
    size_t excess;
    int check_status;
    unsigned status;
} wire_case_t;

static wire_case_t g_cases[MAX_CASES];
static size_t g_case_count;

/* ---------------------------------------------------------- the decoder */

static int hex_digit(unsigned char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
}

/* The wire field's escape grammar, decoded into exact bytes. A backslash
 * introduces one of \r \n \t \0 \\ or \xHH; anything else after a backslash
 * is a corpus error rather than a literal backslash, so a typo is loud. */
static int decode_wire(
    const char *source, size_t source_length,
    unsigned char *out, size_t out_capacity, size_t *out_length) {
    size_t written = 0u;
    size_t index;
    for (index = 0u; index < source_length; ++index) {
        unsigned char byte = (unsigned char)source[index];
        if (byte == '\\') {
            unsigned char escape;
            if (index + 1u >= source_length) return -1;
            escape = (unsigned char)source[++index];
            switch (escape) {
                case 'r': byte = '\r'; break;
                case 'n': byte = '\n'; break;
                case 't': byte = '\t'; break;
                case '0': byte = '\0'; break;
                case '\\': byte = '\\'; break;
                case 'x': {
                    int high;
                    int low;
                    if (index + 2u >= source_length) return -1;
                    high = hex_digit((unsigned char)source[index + 1u]);
                    low = hex_digit((unsigned char)source[index + 2u]);
                    if (high < 0 || low < 0) return -1;
                    byte = (unsigned char)((high << 4) | low);
                    index += 2u;
                    break;
                }
                default:
                    return -1;
            }
        }
        if (written >= out_capacity) return -1;
        out[written++] = byte;
    }
    *out_length = written;
    return 0;
}

static int parse_u64(const char *text, size_t length, uint64_t *out) {
    uint64_t result = 0u;
    size_t index;
    if (!length) return -1;
    for (index = 0u; index < length; ++index) {
        unsigned char byte = (unsigned char)text[index];
        uint64_t digit;
        if (byte < '0' || byte > '9') return -1;
        digit = (uint64_t)(byte - '0');
        if (result > (UINT64_MAX - digit) / 10u) return -1;
        result = result * 10u + digit;
    }
    *out = result;
    return 0;
}

/*
 * The expect column, per conformance/format.md:
 *   accept none|cl=<n>|chunked|until-eof [body=<n>] [excess=<n>] [status=<n>]
 *   reject SYNTAX|FRAMING|LIMIT
 *   incomplete
 */
static int parse_expect(const char *text, wire_case_t *entry) {
    if (strcmp(text, "incomplete") == 0) {
        entry->kind = EXPECT_INCOMPLETE;
        return 0;
    }
    if (strncmp(text, "reject ", 7u) == 0) {
        entry->kind = EXPECT_REJECT;
        if (strcmp(text + 7u, "SYNTAX") == 0) {
            entry->reject = MAELYS_HTTP_ERR_SYNTAX;
        } else if (strcmp(text + 7u, "FRAMING") == 0) {
            entry->reject = MAELYS_HTTP_ERR_FRAMING;
        } else if (strcmp(text + 7u, "LIMIT") == 0) {
            entry->reject = MAELYS_HTTP_ERR_LIMIT;
        } else {
            return -1;
        }
        return 0;
    }
    if (strncmp(text, "accept ", 7u) == 0) {
        const char *cursor = text + 7u;
        int have_framing = 0;
        entry->kind = EXPECT_ACCEPT;
        while (*cursor) {
            const char *end = cursor;
            size_t token_length;
            while (*end && *end != ' ') ++end;
            token_length = (size_t)(end - cursor);
            if (!have_framing) {
                if (token_length == 4u && strncmp(cursor, "none", 4u) == 0) {
                    entry->framing = MAELYS_HTTP_BODY_NONE;
                } else if (token_length > 3u && strncmp(cursor, "cl=", 3u) == 0) {
                    entry->framing = MAELYS_HTTP_BODY_CONTENT_LENGTH;
                    if (parse_u64(cursor + 3u, token_length - 3u,
                                  &entry->content_length) != 0) return -1;
                } else if (token_length == 7u && strncmp(cursor, "chunked", 7u) == 0) {
                    entry->framing = MAELYS_HTTP_BODY_CHUNKED;
                } else if (token_length == 9u && strncmp(cursor, "until-eof", 9u) == 0) {
                    entry->framing = MAELYS_HTTP_BODY_UNTIL_EOF;
                } else {
                    return -1;
                }
                have_framing = 1;
            } else if (token_length > 5u && strncmp(cursor, "body=", 5u) == 0) {
                entry->check_body = 1;
                if (parse_u64(cursor + 5u, token_length - 5u,
                              &entry->body_bytes) != 0) return -1;
            } else if (token_length > 7u && strncmp(cursor, "excess=", 7u) == 0) {
                uint64_t excess;
                if (parse_u64(cursor + 7u, token_length - 7u, &excess) != 0 ||
                    excess > MAX_WIRE_BYTES) return -1;
                entry->excess = (size_t)excess;
            } else if (token_length > 7u && strncmp(cursor, "status=", 7u) == 0) {
                uint64_t status;
                entry->check_status = 1;
                if (parse_u64(cursor + 7u, token_length - 7u, &status) != 0 ||
                    status > 599u) return -1;
                entry->status = (unsigned)status;
            } else {
                return -1;
            }
            cursor = *end ? end + 1u : end;
        }
        return have_framing ? 0 : -1;
    }
    return -1;
}

/* ----------------------------------------------------------- the loader */

static void strip_line_ending(char *line) {
    size_t length = strlen(line);
    while (length > 0u &&
           (line[length - 1u] == '\n' || line[length - 1u] == '\r')) {
        line[--length] = '\0';
    }
}

/*
 * One pass over the corpus file. A case is exactly three lines in order -
 * "name: ", "expect: ", "wire: " - separated by blank lines and documented
 * by '#' comments, both ignored. Any field out of order, any missing field
 * at end of file, and any decode or bound failure is a named, fatal error:
 * a corpus this harness half-read would assert less than it claims.
 */
static int load_corpus(const char *path) {
    static char line[MAX_LINE_BYTES];
    FILE *file = fopen(path, "rb");
    wire_case_t pending;
    int have_name = 0;
    int have_expect = 0;
    unsigned long line_number = 0u;
    int status = 0;
    if (!file) {
        fprintf(stderr, "conformance: cannot open corpus %s\n", path);
        return -1;
    }
    memset(&pending, 0, sizeof(pending));
    while (fgets(line, sizeof(line), file)) {
        size_t raw_length = strlen(line);
        ++line_number;
        if (raw_length == sizeof(line) - 1u && line[raw_length - 1u] != '\n') {
            fprintf(stderr, "conformance: line %lu exceeds %u bytes\n",
                line_number, (unsigned)MAX_LINE_BYTES);
            status = -1;
            break;
        }
        strip_line_ending(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        if (strncmp(line, "name: ", 6u) == 0) {
            if (have_name) {
                fprintf(stderr, "conformance: line %lu: a second name before wire\n",
                    line_number);
                status = -1;
                break;
            }
            memset(&pending, 0, sizeof(pending));
            if (strlen(line + 6u) >= sizeof(pending.name)) {
                fprintf(stderr, "conformance: line %lu: name over %u bytes\n",
                    line_number, (unsigned)MAX_NAME_BYTES);
                status = -1;
                break;
            }
            memcpy(pending.name, line + 6u, strlen(line + 6u) + 1u);
            have_name = 1;
            have_expect = 0;
        } else if (strncmp(line, "expect: ", 8u) == 0) {
            if (!have_name || have_expect) {
                fprintf(stderr, "conformance: line %lu: expect out of order\n",
                    line_number);
                status = -1;
                break;
            }
            if (parse_expect(line + 8u, &pending) != 0) {
                fprintf(stderr, "conformance: line %lu: bad expect \"%s\"\n",
                    line_number, line + 8u);
                status = -1;
                break;
            }
            have_expect = 1;
        } else if (strncmp(line, "wire: ", 6u) == 0) {
            if (!have_name || !have_expect) {
                fprintf(stderr, "conformance: line %lu: wire out of order\n",
                    line_number);
                status = -1;
                break;
            }
            if (decode_wire(line + 6u, strlen(line + 6u), pending.wire,
                            sizeof(pending.wire), &pending.length) != 0) {
                fprintf(stderr, "conformance: line %lu: bad wire escape or overflow\n",
                    line_number);
                status = -1;
                break;
            }
            if (pending.excess > pending.length) {
                fprintf(stderr, "conformance: line %lu: excess over wire length\n",
                    line_number);
                status = -1;
                break;
            }
            if (g_case_count >= MAX_CASES) {
                fprintf(stderr, "conformance: more than %u cases\n",
                    (unsigned)MAX_CASES);
                status = -1;
                break;
            }
            g_cases[g_case_count++] = pending;
            have_name = 0;
            have_expect = 0;
        } else {
            fprintf(stderr, "conformance: line %lu: unrecognized \"%s\"\n",
                line_number, line);
            status = -1;
            break;
        }
    }
    if (status == 0 && (have_name || have_expect)) {
        fprintf(stderr, "conformance: file ends inside a case\n");
        status = -1;
    }
    fclose(file);
    return status;
}

/* ----------------------------------------------------------- the runner */

/*
 * One vector, fed at one chunking. chunk_hint 0 feeds the whole vector at
 * once; any other value feeds in fixed-size steps so a boundary lands
 * mid-token. The verdict, framing, body byte count and unconsumed excess
 * are all compared to what the corpus declares, which is what makes every
 * chunking's agreement the same fact as agreement with the corpus.
 */
static int run_case(const wire_case_t *entry, size_t chunk_hint) {
    maelys_http_parser_t *parser = NULL;
    maelys_http_result_t result = MAELYS_HTTP_AGAIN;
    size_t offset = 0u;
    int failures = 0;
    if (maelys_http_parser_create(MAELYS_HTTP_PARSE_RESPONSE, NULL, NULL, NULL,
                                  &parser) != MAELYS_HTTP_OK) {
        fprintf(stderr, "  [%s] parser_create failed\n", entry->name);
        return 1;
    }
    while (offset < entry->length && result == MAELYS_HTTP_AGAIN) {
        size_t amount = chunk_hint ? chunk_hint : entry->length - offset;
        size_t consumed = 0u;
        if (amount > entry->length - offset) amount = entry->length - offset;
        result = maelys_http_parser_feed(parser, entry->wire + offset, amount,
                                         &consumed);
        if (consumed > amount) {
            fprintf(stderr, "  [%s] consumed more than fed\n", entry->name);
            ++failures;
            break;
        }
        offset += consumed;
        if (result == MAELYS_HTTP_AGAIN && consumed != amount) {
            fprintf(stderr, "  [%s] AGAIN left bytes unconsumed\n", entry->name);
            ++failures;
            break;
        }
    }
    if (failures) {
        maelys_http_parser_release(parser);
        return failures;
    }
    switch (entry->kind) {
        case EXPECT_ACCEPT:
            if (entry->framing == MAELYS_HTTP_BODY_UNTIL_EOF) {
                /* Until-EOF framing never self-completes: every byte is
                 * consumed, the parser holds AGAIN, and only the explicit
                 * EOF completes the message. */
                if (result != MAELYS_HTTP_AGAIN ||
                    offset != entry->length ||
                    maelys_http_parser_body_framing(parser) !=
                        MAELYS_HTTP_BODY_UNTIL_EOF ||
                    maelys_http_parser_eof(parser) != MAELYS_HTTP_COMPLETE) {
                    fprintf(stderr, "  [%s] until-eof contract broken (result %d)\n",
                        entry->name, (int)result);
                    ++failures;
                }
            } else if (result != MAELYS_HTTP_COMPLETE) {
                fprintf(stderr, "  [%s] expected COMPLETE, got %d\n",
                    entry->name, (int)result);
                ++failures;
            } else if (maelys_http_parser_body_framing(parser) != entry->framing) {
                fprintf(stderr, "  [%s] framing %d, want %d\n", entry->name,
                    (int)maelys_http_parser_body_framing(parser),
                    (int)entry->framing);
                ++failures;
            } else if (offset != entry->length - entry->excess) {
                /* The keep-alive stray-byte rule: a complete message consumes
                 * exactly its own bytes, and the excess stays on the wire for
                 * the next read, at every fragmentation. */
                fprintf(stderr, "  [%s] consumed %zu, want %zu\n", entry->name,
                    offset, entry->length - entry->excess);
                ++failures;
            }
            if (!failures &&
                entry->framing == MAELYS_HTTP_BODY_CONTENT_LENGTH &&
                (maelys_http_parser_content_length(parser) != entry->content_length ||
                 maelys_http_parser_body_bytes(parser) != entry->content_length)) {
                fprintf(stderr, "  [%s] length %llu body %llu, want %llu\n",
                    entry->name,
                    (unsigned long long)maelys_http_parser_content_length(parser),
                    (unsigned long long)maelys_http_parser_body_bytes(parser),
                    (unsigned long long)entry->content_length);
                ++failures;
            }
            if (!failures && entry->check_body &&
                maelys_http_parser_body_bytes(parser) != entry->body_bytes) {
                fprintf(stderr, "  [%s] body %llu, want %llu\n", entry->name,
                    (unsigned long long)maelys_http_parser_body_bytes(parser),
                    (unsigned long long)entry->body_bytes);
                ++failures;
            }
            if (!failures && entry->check_status &&
                maelys_http_parser_status(parser) != entry->status) {
                fprintf(stderr, "  [%s] status %u, want %u\n", entry->name,
                    maelys_http_parser_status(parser), entry->status);
                ++failures;
            }
            break;
        case EXPECT_REJECT:
            if (result != entry->reject) {
                fprintf(stderr, "  [%s] expected error %d, got %d\n",
                    entry->name, (int)entry->reject, (int)result);
                ++failures;
            }
            break;
        case EXPECT_INCOMPLETE:
            /* Every byte was fed and the parser neither completed nor
             * refused: a partial response is held, and an EOF here is a
             * truncation, which is a framing error, not a completion. */
            if (result != MAELYS_HTTP_AGAIN || offset != entry->length ||
                maelys_http_parser_eof(parser) != MAELYS_HTTP_ERR_FRAMING) {
                fprintf(stderr, "  [%s] expected incomplete, got %d\n",
                    entry->name, (int)result);
                ++failures;
            }
            break;
    }
    maelys_http_parser_release(parser);
    return failures;
}

int main(int argc, char **argv) {
    static const size_t chunks[] = {0u, 1u, 2u, 3u, 7u, 13u};
    int failures = 0;
    size_t index;
    size_t which;
    if (argc < 2) {
        fprintf(stderr, "usage: %s <corpus.txt>\n", argv[0]);
        return 2;
    }
    if (load_corpus(argv[1]) != 0) return 2;
    if (g_case_count == 0u) {
        fprintf(stderr, "conformance: corpus %s has no cases\n", argv[1]);
        return 2;
    }
    for (index = 0u; index < g_case_count; ++index) {
        for (which = 0u; which < sizeof(chunks) / sizeof(*chunks); ++which) {
            failures += run_case(&g_cases[index], chunks[which]);
        }
    }
    if (failures) {
        fprintf(stderr, "conformance: %d failure(s) over %zu cases\n",
            failures, g_case_count);
        return 1;
    }
    printf("conformance: %zu cases x %zu fragmentations OK\n",
        g_case_count, sizeof(chunks) / sizeof(*chunks));
    return 0;
}
