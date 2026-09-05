# Changelog

## 0.1.5 - 2026-09-05

### Fixed

- Use `mbedtls_ssl_conf_min_tls_version` with Mbed TLS 3.6 and later. The
  compatibility API is deprecated in 3.6.5, so Ubuntu 26.04 correctly failed
  the build under `-Werror` even though TLS 1.2 was configured.

### Changed

- CI and release verification use the GitHub-hosted Ubuntu 26.04 x86_64 and
  arm64 runners, and every `actions/checkout` use is immutably pinned at
  v7.0.1.

## 0.1.4 - 2026-09-05

### Changed

- Pin Maelys System 0.9.0 (`6bd5195`, signed tag `v0.9.0`). The 0.5.0 pin
  named a commit that no longer exists in the public System history, which
  broke every CI checkout. The transport now checks `MAELYS_SYS_ABI_VERSION`
  at compile time.
- The POSIX transport classifies socket I/O by System's result codes
  instead of `errno`: `ERR_WOULD_BLOCK` is readiness, `ERR_CLOSED` is only
  the clean end of stream, and the new `ERR_RESET` fails the exchange. A
  close-delimited response cut by a TCP reset was previously accepted as
  complete.
- Each connection and each DNS wait used a private System reactor; both now
  use `maelys_sys_fd_wait`, one `poll(2)` bounded by the deadline. A
  readiness wait never fails on the `ERROR` indication alone: the I/O call
  that follows carries the cause, so a reset is diagnosed as a reset on
  every host.
- DNS completion is signalled through a System wakeup (an `eventfd` on
  Linux, a pipe elsewhere) instead of a hand-made pipe pair per request.

### Fixed

- Never park a connection whose request head or body was not fully written.
  An early final response (for example `413`) whose body arrived after the
  upload probe could leave a truncated upload parked for reuse, so the next
  request head would have been consumed by the peer as body octets.
- `maelys_http_request_config_create` returns `ERR_ARGUMENT` for a NULL
  target instead of dereferencing it.

## 0.1.3 - 2026-09-03

### Changed

- Relicense from Apache-2.0 to the Mozilla Public License 2.0, the license
  of every Maelys repository. No code change.

## 0.1.2 - 2026-09-01

### Fixed

- Publish and checksum the canonical uncompressed source tar alongside the
  gzip convenience archive, so the source-tree digest is reproducible across
  gzip implementations on macOS and Linux.
- Document that connection reuse never retries a request after any request
  octet may have been written; callers retain responsibility for method-aware
  retry policy after an ambiguous transport failure.

## 0.1.1 - 2026-09-01

### Fixed

- Emit release checksum manifests with archive basenames so they verify after
  GitHub assets are downloaded outside the build tree.

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
