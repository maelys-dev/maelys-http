# Security model

## Guarantees

- HTTP/1.1 only; exact CRLF line endings.
- Bounded start-line, header line/block/count, body, chunk line and trailers.
- `tchar` header names; no whitespace before `:`.
- HTTP/1.1 requests require exactly one `Host` field. The generic parser and
  writer validate a conservative, non-list `uri-host[:port]` subset; the
  client additionally generates it from the validated authority and forbids
  caller duplicates.
  The H2 POSIX client rejects percent-encoded reg-names rather than passing
  encoded text unchanged to DNS.
- Bare LF, obs-fold, NUL, C0 controls other than permitted HTAB, and DEL are
  rejected.
- Multiple `Content-Length` fields are rejected even when identical.
- `Content-Length` plus `Transfer-Encoding` is always rejected.
- Only the single transfer coding `chunked` is accepted.
- Chunk sizes and decimal lengths are checked for integer overflow.
- Trailers are separate from headers and framing fields are forbidden there.
- A parser error is sticky; untrusted bytes are never scanned for a new start.
- Client deadlines use an absolute monotonic deadline.
- Cancellation closes the underlying exchange idempotently.
- Redirects require an application decision. `Authorization`, `Cookie`, and
  `Proxy-Authorization` are stripped whenever scheme or authority changes.
- The shipped Mbed TLS provider requires an explicit trust anchor, TLS 1.2 or
  newer, verifies both chain and hostname, and rejects transport EOF without
  authenticated `close_notify` when HTTP framing depends on connection close.
- Secret header values are never included in library diagnostics.

## Caller responsibilities

Syntax is not policy. Consumers must independently enforce:

- accepted methods, routes, media types and body schemas;
- Host/authority consistency where their threat model needs it;
- Origin, principal, proxy credentials, Registry bearer auth and SSRF policy;
- redirect destinations and downgrade policy;
- response/trailer semantics;
- logging redaction outside this library.

The redirect callback is a policy seam, not a safe-default allowlist. With no
callback, redirects are returned as ordinary responses and never followed.
Third-party TLS providers are normatively required to authenticate the peer and
hostname and to report authenticated closure distinctly, but the generic ABI
cannot mechanically prove a provider's implementation.

## Resolver availability boundary

The POSIX provider bounds its DNS worker pool to four threads and its queue to
64 entries. Exchange cancellation safely releases ownership, but portable
`getaddrinfo()` has neither a deadline nor a cancellation contract: an OS/NSS
resolver that blocks can retain a worker after the exchange deadline. Four
such calls can exhaust this H2 provider until they return; further queue
saturation fails closed. H2 does not claim a hard DNS-availability deadline.
A future provider can move resolution behind a supervised resolver process,
where termination provides the boundary that POSIX threads cannot. The current
private provider seam advertises zero hard guarantees for getaddrinfo; it does
not rename an exchange timeout into DNS cancellation. See
`resolver-provider-design.md`.

## Explicit non-guarantees

No HTTP/2, HTTP/3, HPACK/QPACK, transparent compression, cookies, cache,
authentication, proxy semantics, server listener, DNS pinning, connection pool,
or product-specific security policy is implemented.
