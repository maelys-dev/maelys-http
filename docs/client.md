# Generic client contract

The client API models an exchange, not an OCI download:

```text
method + scheme + authority + target + headers
                         │
              optional streaming body source
                         │
                         ▼
                 transport provider
                         │
                         ▼
status + headers + streaming body sink
```

An optional response-headers callback runs after final headers are parsed and
before any body octet reaches the sink. It may accept, pause with identical
borrowed views replayed later, or fail the exchange. Redirect policy runs first:
a followed intermediate response is suppressed, while a denied redirect is
offered to the headers callback and body sink like any ordinary response.

GET, HEAD and POST are ordinary methods; no method-specific application policy
is embedded. A fixed body must produce exactly its declared octet count. A
chunked body is encoded incrementally and terminated only after the source says
END. A truncated fixed response and malformed chunked response are errors.
CONNECT is intentionally rejected by the H2 client because this ABI has no
tunnel-stream handoff. The standalone H1 codec still validates CONNECT
authority-form requests and can end a successful CONNECT response at its header
boundary for a higher-level tunnel implementation.

`maelys_http_exchange_advance()` performs a configured number of bounded
progress steps. `AGAIN` means either a fairness yield or application
backpressure. The caller knows the latter from its own callback state: it
retries a fairness yield immediately and a callback pause only when that
endpoint is ready. `PAUSE` consumes no bytes; the callback receives the
identical slice again. Each parser step examines at most the configured I/O
buffer size. Network readiness is bounded internally by the absolute deadline
supplied at exchange creation. Individual waits are also time-sliced (50 ms by
default), so even an exchange with an infinite deadline returns control to its
owner for cancellation.

Redirects are disabled unless a callback accepts each hop. Bodies are not
automatically replayed for 301/302/307/308. A 303 may switch to GET. On any
scheme or authority change, sensitive credentials are removed before the next
request regardless of callback behavior.

Before the callback runs, the library resolves a supported `Location` into a
validated `(scheme, authority, origin-form target)` tuple. No transport open,
DNS lookup or connection for the new authority occurs until the callback says
FOLLOW. The supported location forms are absolute HTTP(S) URI, network-path
reference (`//authority/path`) and absolute-path reference (`/path`). Other
relative references (`next`, `../next`) are rejected; query-only references are
also rejected. This is an explicit HTTP/1.1 client subset, not a full RFC 3986
URI resolver.

The client generates exactly one `Host` header from the validated authority.
Callers cannot add `Host`, framing (`Content-Length`, `Transfer-Encoding`) or
hop-switching (`Connection`, `Upgrade`) headers. `101 Switching Protocols` is
rejected because this ABI has no upgraded-stream handoff. Informational
responses are bounded by `max_informational_responses`; each one is also bound
by the ordinary start-line and header limits. HEAD, all 1xx, 204 and 304 have no
message body. The standalone codec can additionally be told that a response is
to CONNECT, in which case a successful response ends at the header boundary.

The same parser start-line, per-header-line, cumulative-header and header-count
limits are applied to the serialized request before the transport is opened.
They count the exact HTTP/1.1 wire bytes, including every CRLF, the generated
`Host`, `Connection` (omitted when connection reuse is enabled) and framing
fields, and the terminating empty line.

This shape deliberately preserves the future MCP HTTPS connector case: POST
request streaming, JSON or long-lived SSE/chunked responses, cancellation and
backpressure fit without introducing MCP semantics into the client.
