# Changelog

## Unreleased

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

This development entry is not a release or compatibility promise.
