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

## Status

Version 0.1.0 establishes public ABI 1 for the H1 codec and H2 generic client
milestones; those milestone names are not an HTTP/2 support claim. Only
HTTP/1.1 is accepted. A future TLS provider that negotiates `h2` must fail
unless an explicit caller policy and an HTTP/2 engine exist; this release
neither advertises nor accepts `h2`.

## Build

The current integration requires Maelys System 0.9 (ABI 1) and pins the
exact commit named by the signed `v0.9.0` tag. The public System history was
restarted on 2026-09-03; only tags published after that date are valid pins.

```sh
git clone https://github.com/maelys-dev/maelys-system.git
git -C maelys-system switch --detach 6bd51950c83eaad9ec16cbac318549ab9bb2e928
git clone https://github.com/maelys-dev/maelys-http.git
make -C maelys-system
make -C maelys-http check SYSTEM_DIR=../maelys-system
make -C maelys-http sanitizers SYSTEM_DIR=../maelys-system
```

The Mbed TLS module is optional:

```sh
make check-mbedtls
```

See [architecture](docs/architecture.md), [security model](docs/security-model.md),
[client contract](docs/client.md),
[resolver provider design](docs/resolver-provider-design.md), and
[provenance](docs/provenance.md).

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
The provenance document records the Maelys implementations and corpora studied
while designing this independent component.
