#!/bin/sh
set -eu

fail() { echo "boundary audit: $*" >&2; exit 1; }

command -v rg >/dev/null 2>&1 || fail 'ripgrep is required for boundary auditing'

if rg -n '#include .*maelys/(egress|executor|mcp|warden)|OCI|JSON-RPC|Proxy-Authorization' \
    include src providers --glob '*.[ch]'; then
    fail 'product policy leaked into the common library'
fi

if rg -n '#include <(sys/socket|netdb|arpa/inet|poll)\.h>|\b(socket|connect|getaddrinfo|recv)\(' \
    src/common.c src/parser.c src/message.c src/tls.c include/maelys/http.h; then
    fail 'socket or connector primitive leaked into the H1 codec'
fi

if rg -n 'src/internal|loop_backend|MAELYS_SYS_INTERNAL' src providers include; then
    fail 'maelys-system private API used'
fi

if rg -n '\b(strcpy|strcat|sprintf|gets)\s*\(' src providers include; then
    fail 'unbounded C string primitive used'
fi

if rg -n '\b(socket|connect|recv|send|getsockopt|shutdown|close)\s*\(' \
    providers/transport_posix.c; then
    fail 'raw POSIX socket lifecycle bypasses maelys-system in transport_posix'
fi

echo 'boundary audit: ok'
