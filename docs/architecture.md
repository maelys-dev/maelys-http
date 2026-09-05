# Architecture

## Boundaries

```text
Application policy
  MCP / Egress / OCI Registry
                │
                ▼
libmaelys-http-client.a
  request exchange, redirects, streaming, cancellation
                │
       ┌────────┴────────┐
       ▼                 ▼
libmaelys-http.a    transport provider
framing codec       fake or POSIX/TLS
       │                 │
       └────────┬────────┘
                ▼
        maelys-system 0.5
      readiness/deadlines/socket
```

`libmaelys-http.a` depends only on the C standard library. It is incremental,
socket-free and unaware of transport. It owns bounded copies of start-lines and
headers; callers receive borrowed views. Body slices are borrowed only during a
synchronous callback.

`libmaelys-http-client.a` builds a generic exchange from method, scheme,
authority, target, headers and a body source. Responses are streamed to a body
sink. The exchange retains data when an application sink pauses, so
backpressure never turns into dropped bytes or unbounded buffering.

The transport seam makes deterministic hostile-server tests possible without
network or TLS. The POSIX transport is a separate object in the client archive.
It uses Maelys System for opaque socket lifecycle, mechanical connect,
partial I/O, shutdown, resolver notification readiness and absolute deadlines.
Address order, retry and TLS state remain in this HTTP transport.

## System 0.5 boundary

System owns only portable POSIX mechanics: nonblocking+CLOEXEC socket creation
and accept, SIGPIPE protection, connect start/completion, partial receive/send,
idempotent shutdown, bind and listen. It knows no hostname, URI, DNS, TLS,
redirect, proxy or HTTP type.

The connector retains `getaddrinfo`, numeric-address ordering, the 32-attempt
ceiling, connection retry and TLS. Four compatibility resolver workers serve a
queue of at most 64 requests; exhaustion fails closed instead of spawning
unbounded threads. Completion reaches the owner thread through a System
wakeup (an `eventfd` on Linux, a pipe elsewhere) that the transport waits on
with `maelys_sys_fd_wait`. Cancellation is memory-safe but cannot interrupt portable
`getaddrinfo()`. The resolver seam is private until the supervised-process
provider described in `resolver-provider-design.md` proves true cancellation,
bounded IPC and worker-death behavior.

## Framing state machine

```text
start-line → headers ─┬─ no body ───────────→ complete
                      ├─ Content-Length ────→ complete
                      ├─ chunk size/data ─┬─→ trailers → complete
                      │                   └─→ next chunk
                      └─ response-until-EOF → complete on EOF

any syntax/framing/limit error → terminal rejected state
```

There is no resynchronization after an error. One parser accepts exactly one
message before reset. `Content-Length` plus `Transfer-Encoding`, multiple
`Content-Length` fields, decimal overflow, multiple transfer codings, and
unsupported transfer codings fail closed.

## TLS

The TLS provider is nonblocking, owner-thread-confined, borrows the socket and
never closes it. Client session creation requires a nonempty server name, used
for SNI and certificate hostname validation. Trust verification is mandatory.
The optional Mbed TLS provider is linked separately.

HTTP/1.1 is the only protocol engine. No provider may silently negotiate `h2`.
ALPN support belongs to a future ABI addition with an explicit fallback policy.

## Connection lifetime

By default the client sends `Connection: close` and closes each exchange. This
is the conservative valid member of the contract “close or reuse only after
complete framing.”

`maelys_http_client_set_connection_reuse` opts a client handle into HTTP/1.1
reuse: requests omit `Connection: close` and a completed exchange may park at
most one idle connection on the handle — a slot, not a pool. The reuse key is
the scheme plus the canonical authority (host lowercased, explicit port
normalized) over the client’s single immutable transport, which carries the
TLS identity. Taking the parked stream empties the slot before an exchange may
touch it, so a second concurrent exchange on the same connection — pipelining
included — is structurally impossible.

Every completion path has a physical gate: the connection is destroyed rather
than parked after EOF-delimited framing, a `close` token in the response
`Connection` list, an incompletely consumed body, bytes beyond the framed
response, any parser or sink error, timeout or cancellation mid-exchange,
`101`, or a redirect (the follow-up dials fresh; the old connection is never
migrated). A parked connection is destroyed at the next reuse attempt when its
key mismatches, `idle_connection_ttl_ms` has elapsed, `max_connection_reuses`
is exhausted, or the idle stream is no longer quiet — readable bytes, EOF
(including a TLS closure without close_notify) or transport failure. All
clocks are monotonic. Multi-connection pooling remains deferred.
