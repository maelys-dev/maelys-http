# Changelog

## Unreleased

### Changed

- Take the maelys-release shared CI while keeping this repository's own
  release workflow, which socle 0.21.0 made possible. The check job reads
  `dependencies/*.pin` and `dependencies/packages` itself, performs the
  pinned checkouts and returns the conventions verdict on three targets; the
  jobs beside it cover the second compiler on both Linux architectures and
  the pinned Mbed TLS.
- Declare Mbed TLS as `dependencies/mbedtls.pin`, with the `repository` line
  socle 0.21.0 added for a dependency outside `maelys-dev`. Neither pinned
  commit is written out anywhere else now: both workflows check their
  dependencies out through `scripts/checkout-dependency.sh`, which reads the
  pins, and the Mbed TLS submodule is initialised beside it since a pin
  cannot reach it.
- Extract the release-time verification into `scripts/verify-release.sh
  TARGET`, the hook socle 0.21.0 defined. The release workflow calls it on
  each target instead of carrying the commands inline, so adopting the
  socle's release later needs no rewriting of what it verifies.
- Rename the fuzz targets to the fleet convention: `make fuzz` is the
  libFuzzer campaign and `make fuzz-smoke` replays the committed corpus in
  seconds without any sanitizer runtime. `make check` depends on the latter
  and the shared CI runs it as its `fuzz_command`.
- Adopt the conventions files the socle writes: `AGENTS.md`, `CLAUDE.md` and
  `LICENSING.md`. `maelys-release check` now passes.
- Move to socle 0.24.0 and declare the Mbed TLS submodule on a `submodules`
  line of its pin, which 0.22.0 added after this repository reported that
  `cmake` refuses to configure without it. The line the workflows carried to
  fetch it by hand is gone: the managed script alone now leaves a tree that
  configures. 0.22.1 also stops a broken third-party apt source of the
  runner image from failing a product's Linux jobs.

## 0.1.10 - 2026-09-09

### Changed

- Declare the Maelys System dependency the way the maelys-release socle does,
  as `dependencies/maelys-system.pin` with the nearest tag on line 1 and the
  pinned commit on line 2, and adopt the socle's managed
  `scripts/checkout-dependency.sh`. The commit was written out in six places;
  the Makefile, the SPDX SBOM and all five workflow checkouts now read the one
  declaration, so a re-pin is a single edit. `deps/MAELYS_SYSTEM_PIN` is gone.
- Declare the runner packages in `dependencies/packages`, which the socle
  reads. Mbed TLS is deliberately absent from the Linux section: the floor in
  `providers/mbedtls_version_policy.h` is above what the distribution ships,
  so CI keeps building a pinned upstream commit.

## 0.1.9 - 2026-09-09

### Fixed

- libFuzzer no longer writes into the committed fuzz corpus. It saves what it
  discovers into the first corpus directory it is given, and that was the one
  in the source tree, so a local campaign left the checkout dirty. The
  writable corpus is now in the build tree and the seeds are only ever read,
  as Egress and OCI already did.

### Changed

- Move `fuzz/` to `tests/fuzz/`, the layout Egress and OCI use, and add a
  README covering the entry points, the seed corpus, the campaign budget and
  the absent libFuzzer runtime on Apple Clang.
- Replay the fuzz seeds under ASan and UBSan in `make sanitizers`. That is the
  deepest fuzz gate a host without libFuzzer can run, and `make check` only
  replayed them uninstrumented.
- Raise the default libFuzzer budget to 10000 runs per entry point, matching
  Egress, and bound generated inputs to 65536 octets, matching OCI. Both are
  overridable through `FUZZ_RUNS` and `FUZZ_MAX_LEN` for a longer campaign.

## 0.1.8 - 2026-09-09

### Security

- Reject forbidden `Content-Length` and `Transfer-Encoding` fields on a 2xx
  response to CONNECT, as already done for 1xx and 204. RFC 9110 section 9.3.6
  forbids them identically, and the octets such a field claims are the first
  bytes of the tunnel. A CONNECT response that failed keeps its ordinary body.
- Bound chunk extensions over the whole message. `max_chunk_line_bytes` bounds
  one chunk line and never their sum, and a data chunk carries at least one
  body octet, so a peer could pad every chunk of a body-sized stream with
  extensions the recipient must ignore and spend a kilobyte of wire per useful
  octet. Their total is now held to the same budget the header block has. The
  size lines stay unmetered, so a long-lived chunked or SSE response, which is
  many small chunks and no extension, is unaffected.

### Changed

- Pin Maelys System 0.9.1 (`6663c83`, signed tag `v0.9.1`). It reports a peer's
  reset as `ERR_RESET` on the sending side on macOS, where an upload cut by a
  reset was diagnosed as a clean peer close, and stops `fd_wait` from
  reporting a timeout that has not come.
- A `Location` outside the supported subset, or one whose authority the client
  refuses, no longer fails the exchange. The redirect is left unfollowed and
  the response is delivered like a 3xx carrying no `Location` at all, so the
  caller still sees the status and the header instead of losing the response
  to a syntax error. The policy callback is never offered a destination the
  client could not resolve, and nothing is dialled for it. Two `Location`
  fields remain a framing error.

### Fixed

- Take the maelys-system version in the SPDX SBOM from the pinned dependency
  instead of a hard-coded string, so a re-pin cannot leave the document
  naming a version the recorded commit does not carry. Mbed TLS, which the
  consumer resolves, no longer carries a sentence where SPDX expects a
  version.

## 0.1.7 - 2026-09-06

### Fixed

- Make the whitespace audit work from a source archive without Git metadata
  when ripgrep is unavailable.

## 0.1.6 - 2026-09-06

No artifact was published under this version and its tag has been removed: the
release run failed after the gates, on the whitespace audit inside the source
archive. Everything below reached the public in 0.1.7, which carries the fix
for that audit.

### Security

- Reject forbidden `Content-Length` and `Transfer-Encoding` fields on 1xx and
  204 responses before a connection can be reused.
- Enforce maintained Mbed TLS security floors at compile time and client
  creation, with an explicit and narrowly scoped acknowledgement for
  distribution-backported fixes.
- Verify release tag and commit authenticity, promote the exact tested archive,
  and publish SPDX SBOM, provenance and offline Sigstore bundles.

### Changed

- Put repository and pinned-dependency headers before environment include paths
  and make the dependency-pin check part of normal and sanitizer validation.
- Add generic absolute-form, CONNECT, bare-CR and unbracketed-IPv6 request
  regressions adapted from the current Maelys Egress parser tests.
- Align stale System boundary documentation with the pinned 0.9.0 dependency.

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
