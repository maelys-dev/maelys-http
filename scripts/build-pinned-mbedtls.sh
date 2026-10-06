#!/bin/sh
# Builds and installs the Mbed TLS commit dependencies/mbedtls.pin names, and
# prints the pkg-config directory of the result. The Linux distributions ship
# Mbed TLS below the floor providers/mbedtls_version_policy.h enforces, so
# every Linux gate — the release, and the mbedtls job of ci.yml — reads the
# pinned commit instead, through this one script rather than through two
# copies of the same cmake invocation.
#
# usage: eval "$(sh scripts/build-pinned-mbedtls.sh [NAME])"   # sets PKG_CONFIG_PATH
#    or: dir=$(sh scripts/build-pinned-mbedtls.sh --print [NAME])
#
# NAME is the pin to build, the file name of dependencies/NAME.pin: `mbedtls`
# by default, the 3.6 line every Linux gate and the release read, or
# `mbedtls-4`, the 4.x line a second CI job reads so that both lines
# providers/mbedtls_version_policy.h accepts are built from source on Linux.
#
# The source and the prefix both live under $MAELYS_DEPENDENCIES_DIR, beside
# the other pinned checkouts and never inside this repository, so `make clean`
# does not throw away a build that takes minutes. An already-installed prefix
# is kept: a second call costs nothing.
set -eu

mode=--shell
case "${1:-}" in
    --shell|--print) mode=$1; shift ;;
esac
name=${1:-mbedtls}
case "$name" in
    mbedtls|mbedtls-[0-9]*) ;;
    *) echo "build-pinned-mbedtls: unknown pin: $name" >&2; exit 64 ;;
esac
test -f "$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)/dependencies/$name.pin" || {
    echo "build-pinned-mbedtls: no dependencies/$name.pin" >&2; exit 66
}

root=${MAELYS_DEPENDENCIES_DIR:-}
test -n "$root" || {
    echo 'build-pinned-mbedtls: MAELYS_DEPENDENCIES_DIR is unset; the pinned checkouts live apart' >&2
    echo '  eval "$(sh scripts/checkout-dependencies.sh "$PWD/../maelys-http-deps")"' >&2
    exit 64
}
source="$root/$name"
test -d "$source" || {
    echo "build-pinned-mbedtls: no Mbed TLS checkout at $source" >&2
    exit 66
}
prefix="$root/$name-prefix"

if [ ! -f "$prefix/lib/pkgconfig/mbedtls.pc" ]; then
    cmake -S "$source" -B "$root/$name-build" \
        -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" >&2
    cmake --build "$root/$name-build" --parallel >&2
    cmake --install "$root/$name-build" >&2
fi
test -f "$prefix/lib/pkgconfig/mbedtls.pc" || {
    echo "build-pinned-mbedtls: $prefix carries no mbedtls.pc after the install" >&2
    exit 70
}

if [ "$mode" = --print ]; then
    printf '%s\n' "$prefix/lib/pkgconfig"
else
    printf 'PKG_CONFIG_PATH=%s${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}\n' "$prefix/lib/pkgconfig"
    printf 'export PKG_CONFIG_PATH\n'
fi
