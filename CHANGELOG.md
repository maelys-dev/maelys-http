# Changelog

## 0.1.0 - 2026-09-01

### Added

- H1 socket-free bounded HTTP/1.1 request/response codec, chunked framing,
  separate trailers, writers, adversarial corpus and fragmentation oracles.
- H2 generic streaming HTTP/1.1 client, opaque transport/TLS seams, deadlines,
  cancellation, redirect credential stripping, POSIX transport, and optional
  Mbed TLS client provider.
- Public ABI 1, C++17-compatible headers, packaging metadata and CI gates.
- Consume the additive Maelys System 0.5 opaque socket lifecycle instead of
  duplicating socket/connect/recv/shutdown mechanics in the POSIX transport.
- Add a private injectable resolver seam with bounded numeric results and
  strict response identity validation. The default getaddrinfo provider
  advertises no hard deadline or hard-cancellation guarantee.
- Use direct C11 static initialization for the resolver request counter so
  strict Xcode 26 builds do not depend on deprecated `ATOMIC_VAR_INIT`.
- Opt-in HTTP/1.1 connection reuse per client handle
  (`maelys_http_client_set_connection_reuse`): requests omit
  `Connection: close` and one idle connection is parked, keyed by scheme plus
  canonical authority, bounded by the new `max_connection_reuses` (64) and
  `idle_connection_ttl_ms` (30000) client limits, and destroyed on every
  completion path that is not fully framed and quiet. The default still sends
  `Connection: close` on every request.
- Exercise connection reuse over the real POSIX transport and Mbed TLS, proving
  two requests on one TLS connection and fresh dialing after both an explicit
  `Connection: close` and an otherwise silent peer shutdown. Run the adversarial
  response corpus under ASan/UBSan in addition to the ordinary check suite.

This release establishes public ABI 1 for the codec, client, transport and TLS
provider seams.
