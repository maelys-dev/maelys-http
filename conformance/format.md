# Wire-conformance case-file format

This directory is the single home of the HTTP wire-conformance corpus.
Two files share one physical format and are consumed by two harnesses:

- `request-wire-cases.txt` — adversarial HTTP/1.1 REQUEST vectors, asserted
  against the mcp-runtime server parser (`host/http_parser.c` there). The
  mcp-runtime build points at this checkout (`HTTP_DIR`) and pins the file's
  SHA-256, so edits here are deliberate on both sides.
- `response-wire-cases.txt` — adversarial HTTP/1.1 RESPONSE vectors, asserted
  against this library's parser (`src/parser.c`) by
  `tests/test_conformance.c`.

## Physical format

A case is exactly three fields, one per physical line, in this order,
separated from the next case by blank lines:

    name: <the rule or property this vector exercises>
    expect: <the verdict, see the vocabularies below>
    wire: <the escaped bytes the peer puts on the socket>

Lines starting with `#` are comments; blank lines separate cases. Both are
ignored by the loaders. Any field out of order, any missing field at end of
file, and any decode failure is a fatal load error, never a skipped case.

## The wire field

The wire field is the EXACT byte sequence on the socket, spelled with escapes
so that no literal CR, LF or NUL ever appears in this file and no editor or
git CRLF rule can silently rewrite a vector. A backslash introduces one of:

    \r \n \t \0 \\ \xHH

Anything else after a backslash is a corpus error, so a typo is loud. Every
byte value is expressible via `\xHH`. A vector that must BEGIN with a space
or tab spells it `\x20` / `\t` so the `wire: ` separator stays unambiguous.

## Verdict vocabulary — request corpus

    expect: accept nocl
    expect: accept cl=<n>
    expect: reject <BAD_REQUEST|SMUGGLING|HEADERS_TOO_LARGE|VERSION>
    expect: incomplete

The reject names are the request parser's own reject enum. `incomplete`
means every byte was fed and the parser still holds the partial request.

## Verdict vocabulary — response corpus

    expect: accept none [status=<n>] [excess=<n>]
    expect: accept cl=<n> [excess=<n>]
    expect: accept chunked [body=<n>] [excess=<n>]
    expect: accept until-eof [body=<n>]
    expect: reject <SYNTAX|FRAMING|LIMIT>
    expect: incomplete

The reject names are `MAELYS_HTTP_ERR_SYNTAX`, `MAELYS_HTTP_ERR_FRAMING` and
`MAELYS_HTTP_ERR_LIMIT`, by their short names. The framing word after
`accept` is the parser's established body framing:

- `none` — complete at end of headers with no body (1xx, 204, 304).
- `cl=<n>` — complete under Content-Length framing; `n` is both the declared
  length and the number of body bytes delivered.
- `chunked` — complete under chunked framing; `body=<n>` asserts the total
  de-chunked body bytes.
- `until-eof` — the headers established read-until-close framing: after
  every byte is fed the parser still reports AGAIN, and only
  `maelys_http_parser_eof()` completes it. `body=<n>` asserts the body bytes
  delivered before EOF.
- `excess=<n>` — the keep-alive stray-byte rule: the message is COMPLETE and
  exactly the last `n` bytes of the vector were NOT consumed. They belong to
  the next message on the connection (or are stray). Omitted means 0: a
  complete vector is consumed exactly.
- `incomplete` — every byte was fed, the parser reports AGAIN, and an EOF at
  this point is a framing error (truncation), not a completion.

## What belongs here

Every vector is WIRE BYTES and its verdict is the PARSER's verdict — framing,
start-line and header grammar only. Vectors must not depend on out-of-band
parser configuration (HEAD/CONNECT context, custom limits): the harnesses
run every vector against a default-limits parser in its default mode.
Behavioral scenarios — reuse across requests, timeouts, TLS, redirects,
anything needing a client, a socket or configuration — belong in the unit
tests, not here.

Both harnesses assert every vector at multiple chunk fragmentations with
split equivalence: whole, and fixed steps of 1, 2, 3, 7 and 13 bytes must
all produce the identical verdict.
