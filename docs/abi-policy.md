# ABI policy

The public C ABI starts at version 1. Public state is represented by opaque
handles. Constructors copy strings, operation tables and caller-owned
configuration. Accessors return borrowed views whose lifetime is documented by
the owning handle.

Rules:

- `MAELYS_HTTP_ABI_VERSION` and `MAELYS_HTTP_CLIENT_ABI_VERSION` are
  compile-time consumer contract identifiers. The operation-table identifiers
  `MAELYS_HTTP_TRANSPORT_ABI_VERSION` and `MAELYS_HTTP_TLS_ABI_VERSION` are
  checked for exact equality at provider creation.
- An incompatible operation table requires a new ABI and constructor.
- Existing struct layouts are never silently extended in the 0.x line.
- Result enum values and ownership rules are part of the ABI.
- C headers must compile as C++17 with no adapter header.
- Core, client and optional TLS modules stay separate archives.

The project is pre-1.0. Any ABI event is called out in the changelog and never
introduced through an unversioned provider table.
