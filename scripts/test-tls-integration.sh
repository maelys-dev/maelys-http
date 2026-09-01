#!/bin/sh
set -eu

root="$(mktemp -d)"
ready="$root/ready"
report="$root/report"
port=
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

python3 tests/https_server.py 0 "$root/server.crt" "$root/server.key" "$ready" "$report" \
    >"$root/server.log" 2>&1 &
server_pid=$!
i=0
while [ "$i" -lt 300 ]; do
    if [ -s "$ready" ]; then
        break
    fi
    if ! kill -0 "$server_pid" 2>/dev/null; then
        cat "$root/server.log" >&2
        echo 'TLS integration server exited before readiness' >&2
        exit 1
    fi
    i=$((i + 1))
    sleep .1
done
if [ "$i" -ge 300 ]; then
    cat "$root/server.log" >&2
    echo 'TLS integration server readiness timed out' >&2
    exit 1
fi
port="$(sed -n '1p' "$ready")"
case "$port" in
    ''|*[!0-9]*) echo 'invalid TLS integration port' >&2; exit 1 ;;
esac

build/tls_integration_client "$root/server.crt" "localhost:$port"
build/tls_integration_client "$root/server.crt" "localhost:$port" reuse
build/tls_integration_client "$root/server.crt" "localhost:$port" close
build/tls_integration_client "$root/server.crt" "localhost:$port" silent-close

i=0
while [ "$i" -lt 100 ]; do
    lines="$(wc -l <"$report" | tr -d ' ')"
    if [ "$lines" -ge 7 ]; then
        break
    fi
    i=$((i + 1))
    sleep .05
done
if [ "$i" -ge 100 ]; then
    cat "$root/server.log" >&2
    cat "$report" >&2
    echo 'TLS keep-alive observations timed out' >&2
    exit 1
fi

# Line 1 is the conservative single exchange. The reuse pair must share one
# physical TCP/TLS connection; Connection: close and a silent peer shutdown
# must each force a fresh connection for their second exchange.
if ! awk '
    NR == 2 { reuse = $1; ok = ($2 == "/one") }
    NR == 3 { ok = ok && $1 == reuse && $2 == "/two" }
    NR == 4 { closing = $1; ok = ok && $2 == "/close" }
    NR == 5 { ok = ok && $1 != closing && $2 == "/two" }
    NR == 6 { silent = $1; ok = ok && $2 == "/silent-close" }
    NR == 7 { ok = ok && $1 != silent && $2 == "/two" }
    END { exit ok ? 0 : 1 }
' "$report"; then
    cat "$report" >&2
    echo 'TLS keep-alive connection identity invariant failed' >&2
    exit 1
fi
if build/tls_integration_client "$root/server.crt" "127.0.0.1:$port"; then
    echo 'hostname mismatch was accepted' >&2
    exit 1
fi
if build/tls_integration_client "$root/other.crt" "localhost:$port"; then
    echo 'unknown authority was accepted' >&2
    exit 1
fi
echo 'TLS integration: keep-alive, peer-close, CA, hostname and authority gates passed'
