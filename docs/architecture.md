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
unbounded threads. Completion reaches the owner reactor through a private
nonblocking pipe. Cancellation is memory-safe but cannot interrupt portable
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

The initial client sends `Connection: close` and closes each exchange. This is
the conservative valid member of the contract “close or reuse only after
complete framing.” Connection pooling is deferred until reuse keys include the
full TLS/policy identity and every completion path has a physical gate.
