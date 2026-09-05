# Provenance

Maelys HTTP is an independent MPL-2.0 implementation informed by two
Maelys codebases that were MIT-licensed when studied. It does not link either product and does not
import their product policy.

## MCP parser and corpus

- Repository: `maelys-dev/maelys-mcp` (local checkout named `mcp-runtime`).
- Files studied: `host/http_parser.c`, `host/http_parser.h`,
  `tests/test_http_parser.c`, `fuzz/fuzz_http_request.c`, and
  `fuzz/fuzz_http_smuggling.c`.
- Parser introduction commit: `7695143` (21 August 2026).
- License: MIT, Copyright (c) 2026 David Bromberg.

Reused architectural ideas are the socket-free incremental state machine,
length-explicit slices, overflow-checked decimal framing, sticky rejection,
fragmentation oracle, and CL/TE-biased fuzz corpus. MCP-specific Host, Origin,
route, media type, authentication, JSON-RPC and status policy remain outside.

## Egress parser and TLS seam

- Repository: `maelys-dev/maelys-egress`.
- Files studied: `src/http.c`, `fuzz/fuzz_http.c`,
  `include/maelys/egress_tls.h`, `providers/tls_mbedtls.c`, and tests.
- HTTP parser introduction commit: `6369c61` (23 August 2026), later evolved
  through `6272c5c`.
- License: MIT, Copyright (c) 2026 Maelys Developers.

Reused ideas are hostile proxy cases, an opaque nonblocking TLS provider with
WANT_READ/WANT_WRITE, SNI/hostname verification, and a separately linked TLS
module. CONNECT, absolute-form routing, proxy credentials, principals,
allowlists and relay policy remain outside.

## Maelys System dependency

The H2 client consumes the Maelys System 0.9 socket, descriptor wait, wakeup
and deadline APIs. System remains ABI 1. The signed
`v0.9.0` release commit
`6bd51950c83eaad9ec16cbac318549ab9bb2e928` is recorded in
`deps/MAELYS_SYSTEM_PIN`, the Makefile and CI. No internal System symbol is
consumed.

The source attributions above are retained even though the new implementation
was written for this repository rather than copied as a wholesale file.
