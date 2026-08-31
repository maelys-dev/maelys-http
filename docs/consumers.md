# Consumer boundaries

## Warden OCI (planned 0.43)

Warden may pin a released Maelys HTTP ABI after its 0.42 CAS contract is
stable. Maelys HTTP supplies HTTPS exchange mechanics only:

```text
Registry protocol policy (Warden)
  bearer challenge, media types, descriptor graph, expected size/digest
                              │
                              ▼
Maelys HTTP exchange
  GET/HEAD, redirect callback, bounded framing, TLS, streamed body
                              │
                              ▼
Warden CAS sink
  byte count + SHA-256 + atomic publication
```

Warden must configure the response-body limit from its trusted acquisition
budget; the 64 MiB library default is not an OCI layer-size promise. The HTTP
client never decides whether a media type, registry, redirect or bearer token
is trusted, and it never publishes a CAS object.

## MCP runtime and connector

The codec can replace request/response syntax incrementally after a differential
compatibility cycle. MCP Host, Origin, authentication and JSON-RPC remain in the
runtime. The future HTTPS connector uses generic POST and response streaming;
it must reconstruct outbound credentials and enforce an SSRF destination policy.

## Maelys Egress

Egress may consume the syntax codec and TLS seam, but retains CONNECT,
absolute-form, proxy authentication, allowlist, DNS pinning, receipts and relay
semantics. Maelys HTTP is never a second egress policy engine.
