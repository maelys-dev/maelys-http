# Maelys HTTP

Maelys HTTP is a bounded HTTP/1.1 codec and streaming client written in C11.
It provides the common syntax and transport machinery needed by Maelys MCP,
Maelys Egress, Warden's OCI registry client, and a future MCP HTTPS connector
without importing any of their product policy.

```text
                              maelys-http
                 ┌──────────────┼──────────────┐
                 ▼              ▼              ▼
          MCP HTTP server   Maelys Egress   Warden OCI
                 │
                 ▼
          future MCP HTTPS connector
```

The package deliberately ships separate static archives:

- `libmaelys_http.a`: socket-free HTTP/1.1 parser and writer;
- `libmaelys_http_client.a`: generic streaming client and POSIX transport;
- `libmaelys_http_tls_mbedtls.a`: optional client TLS provider.

## Install

- **Homebrew**: `brew install maelys-dev/tap/libmaelys-http`, with bottles for
  macOS arm64.
- **Binary archive for macOS arm64**: each release publishes
  `maelys-http-VERSION-macos-arm64.tar.gz`, the three static archives, the
  headers and the pkg-config files under `usr/local`, and no shared library.
  It is linked with maelys-system's archive of the version
  `maelys-http-client.pc` requires, and with Homebrew's Mbed TLS: the TLS
  provider was compiled against that build, of the minor version its
  pkg-config file accepts, and another configuration would not have the same
  structure sizes. `usr/local/share/maelys-http/build-info` records both
  versions. [RELEASING.md](RELEASING.md) says how the archive is checked and
  how to verify its provenance.
- **Source**: `maelys-http-VERSION.tar.gz`, built as below.

## Status

Version 0.1.0 establishes public ABI 1 for the H1 codec and H2 generic client
milestones; those milestone names are not an HTTP/2 support claim. Only
HTTP/1.1 is accepted. A future TLS provider that negotiates `h2` must fail
unless an explicit caller policy and an HTTP/2 engine exist; this release
neither advertises nor accepts `h2`.

## Build

The current integration requires Maelys System (ABI 1) at the exact commit
`dependencies/maelys-system.pin` names: the signed tag on its first line, the
commit that tag points at on its second. The public System history was
restarted on 2026-09-03; only tags published after that date are valid pins.

The pinned checkouts live apart from this repository, never beside it, and one
line both writes them and says where they are:

```sh
git clone https://github.com/maelys-dev/maelys-http.git
cd maelys-http
eval "$(sh scripts/checkout-dependencies.sh "$PWD/../maelys-http-deps")"
make check
make sanitizers
```

The Mbed TLS module is optional. Its security floor is, for each maintained
line, the release that carries the fixes of the July 2026 security advisories:
3.6.7 on the 3.6 LTS line, 4.1.1 on the 4.1 LTS line, and 4.2.0 on the 4.2
line:

```sh
make check-mbedtls
```

A distribution that keeps an older upstream version number while backporting
all applicable security fixes must opt in explicitly with
`CPPFLAGS=-DMAELYS_HTTP_MBEDTLS_ALLOW_BACKPORTED_SECURITY_FIXES=1` and set
`MBEDTLS_PKGCONFIG_MIN_VERSION` to its patched package version when installing.
The exception only lowers the patch floor on the 3.6 and 4.1 lines; it does not
re-enable obsolete major or minor releases.

## Non-goals

This library contains no JSON, JSON-RPC, MCP, OCI Registry, proxy policy,
authentication policy, listener, application/server worker pool, cookie jar,
compression, HTTP/2, or product logging. The POSIX connector alone owns a
bounded four-worker compatibility DNS resolver pool with a queue of 64;
saturation fails closed, but blocking `getaddrinfo()` calls cannot be killed.
The private resolver seam and supervised-process design are documented without
claiming that hard cancellation exists today. Callers own product decisions.

## License

Mozilla Public License 2.0 ([LICENSE](LICENSE)), like every Maelys repository.
[`NOTICE`](NOTICE) carries the retained MIT notices of the codebases studied
while designing this independent component, and
[`docs/provenance.md`](docs/provenance.md) records the exact repositories,
commits and extracted ideas behind them. [`LICENSING.md`](LICENSING.md) states
what each part is licensed under.

## Documentation

The architecture, the security model, the client contract and the resolver
provider design are not kept in this repository; they are published with the
documentation of the other Maelys products. What stays here is what a machine
writes and what this repository engages publicly, which `LICENSING.md` names.
