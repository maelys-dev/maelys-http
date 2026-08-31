#!/bin/sh
set -eu

root="$(mktemp -d)"
cleanup() { rm -rf "$root"; }
trap cleanup EXIT INT TERM

system_dir="${SYSTEM_DIR:-../maelys-system}"
make -C "$system_dir" install DESTDIR="$root" PREFIX=/usr/local >/dev/null
make install DESTDIR="$root" PREFIX=/usr/local >/dev/null
test -f "$root/usr/local/lib/libmaelys_http.a"
test -f "$root/usr/local/lib/libmaelys_http_client.a"
test -f "$root/usr/local/include/maelys/http.h"
test -f "$root/usr/local/include/maelys/http_client.h"
test -f "$root/usr/local/lib/pkgconfig/maelys-http.pc"
test -f "$root/usr/local/lib/pkgconfig/maelys-http-client.pc"

cat >"$root/smoke.c" <<'EOF'
#include <maelys/http.h>
#include <maelys/http_client.h>
int main(void) {
    maelys_http_limits_t limits;
    maelys_http_client_limits_t client_limits;
    maelys_http_limits_default(&limits);
    maelys_http_client_limits_default(&client_limits);
    return limits.max_header_count == 0u || client_limits.io_buffer_bytes == 0u;
}
EOF
${CC:-cc} -std=c11 -Wall -Wextra -Werror \
    -I"$root/usr/local/include" "$root/smoke.c" \
    "$root/usr/local/lib/libmaelys_http_client.a" \
    "$root/usr/local/lib/libmaelys_http.a" \
    "$root/usr/local/lib/libmaelys_sys.a" \
    -pthread -o "$root/smoke"
"$root/smoke"
PKG_CONFIG_PATH="$root/usr/local/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
    pkg-config --validate maelys-http
PKG_CONFIG_SYSROOT_DIR="$root" \
PKG_CONFIG_PATH="$root/usr/local/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
    ${CC:-cc} -std=c11 -Wall -Wextra -Werror "$root/smoke.c" \
    $(PKG_CONFIG_SYSROOT_DIR="$root" \
      PKG_CONFIG_PATH="$root/usr/local/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
      pkg-config --static --cflags --libs maelys-http-client) \
    -o "$root/smoke-pc"
"$root/smoke-pc"

make uninstall DESTDIR="$root" PREFIX=/usr/local >/dev/null
test ! -e "$root/usr/local/lib/libmaelys_http.a"
test ! -e "$root/usr/local/lib/libmaelys_http_client.a"
test ! -e "$root/usr/local/include/maelys/http.h"
echo 'install check: ok'
