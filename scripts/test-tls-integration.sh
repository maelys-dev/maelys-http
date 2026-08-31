#!/bin/sh
set -eu

root="$(mktemp -d)"
port=38443
server_pid=
cleanup() {
    if [ -n "$server_pid" ]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -rf "$root"
}
trap cleanup EXIT INT TERM

openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
    -subj /CN=localhost -addext subjectAltName=DNS:localhost \
    -keyout "$root/server.key" -out "$root/server.crt" >/dev/null 2>&1
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
    -subj /CN=other -addext subjectAltName=DNS:other \
    -keyout "$root/other.key" -out "$root/other.crt" >/dev/null 2>&1

python3 tests/https_server.py "$port" "$root/server.crt" "$root/server.key" &
server_pid=$!
i=0
while [ "$i" -lt 50 ]; do
    if python3 -c 'import socket,sys; s=socket.socket(); s.settimeout(.1); s.connect(("127.0.0.1",int(sys.argv[1]))); s.close()' "$port" 2>/dev/null; then
        break
    fi
    i=$((i + 1))
    sleep .1
done
test "$i" -lt 50

build/tls_integration_client "$root/server.crt" "localhost:$port"
if build/tls_integration_client "$root/server.crt" "127.0.0.1:$port"; then
    echo 'hostname mismatch was accepted' >&2
    exit 1
fi
if build/tls_integration_client "$root/other.crt" "localhost:$port"; then
    echo 'unknown authority was accepted' >&2
    exit 1
fi
echo 'TLS integration: CA, hostname and unknown-authority gates passed'
