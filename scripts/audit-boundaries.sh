#!/bin/sh
set -eu

fail() { echo "boundary audit: $*" >&2; exit 1; }

search_tree() {
    pattern=$1
    shift
    if command -v rg >/dev/null 2>&1; then
        rg -n "$pattern" "$@" --glob '*.[ch]'
    else
        grep -EnR --include='*.c' --include='*.h' "$pattern" "$@"
    fi
}

search_files() {
    pattern=$1
    shift
    if command -v rg >/dev/null 2>&1; then
        rg -n "$pattern" "$@"
    else
        grep -En "$pattern" "$@"
    fi
}

if search_tree '#include .*maelys/(egress|executor|mcp|warden)|OCI|JSON-RPC|Proxy-Authorization' \
    include src providers; then
    fail 'product policy leaked into the common library'
fi

if search_files '#include <(sys/socket|netdb|arpa/inet|poll)\.h>|(^|[^[:alnum:]_])(socket|connect|getaddrinfo|recv)\(' \
    src/common.c src/parser.c src/message.c src/tls.c include/maelys/http.h; then
    fail 'socket or connector primitive leaked into the H1 codec'
fi

if search_tree 'src/internal|loop_backend|MAELYS_SYS_INTERNAL' src providers include; then
    fail 'maelys-system private API used'
fi

if search_tree '(^|[^[:alnum:]_])(strcpy|strcat|sprintf|gets)[[:space:]]*\(' src providers include; then
    fail 'unbounded C string primitive used'
fi

if search_files '(^|[^[:alnum:]_])(socket|connect|recv|send|getsockopt|shutdown|close)[[:space:]]*\(' \
    providers/transport_posix.c; then
    fail 'raw POSIX socket lifecycle bypasses maelys-system in transport_posix'
fi

echo 'boundary audit: ok'
