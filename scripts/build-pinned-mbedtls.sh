#!/bin/sh
# Builds and installs the Mbed TLS commit dependencies/mbedtls.pin names, and
# prints the pkg-config directory of the result. The Linux distributions ship
# Mbed TLS below the floor providers/mbedtls_version_policy.h enforces, so
# every Linux gate — the release, and the mbedtls job of ci.yml — reads the
# pinned commit instead, through this one script rather than through two
# copies of the same cmake invocation.
#
# usage: eval "$(sh scripts/build-pinned-mbedtls.sh)"   # sets PKG_CONFIG_PATH
#    or: dir=$(sh scripts/build-pinned-mbedtls.sh --print)
#
# The source and the prefix both live under $MAELYS_DEPENDENCIES_DIR, beside
# the other pinned checkouts and never inside this repository, so `make clean`
# does not throw away a build that takes minutes. An already-installed prefix
# is kept: a second call costs nothing.
set -eu

mode=${1:---shell}
case "$mode" in
    --shell|--print) ;;
    *) echo "build-pinned-mbedtls: unknown argument: $mode" >&2; exit 64 ;;
esac

root=${MAELYS_DEPENDENCIES_DIR:-}
test -n "$root" || {
    echo 'build-pinned-mbedtls: MAELYS_DEPENDENCIES_DIR is unset; the pinned checkouts live apart' >&2
    echo '  eval "$(sh scripts/checkout-dependencies.sh "$PWD/../maelys-http-deps")"' >&2
    exit 64
}
source="$root/mbedtls"
test -d "$source" || {
    echo "build-pinned-mbedtls: no Mbed TLS checkout at $source" >&2
    exit 66
}
prefix="$root/mbedtls-prefix"

if [ ! -f "$prefix/lib/pkgconfig/mbedtls.pc" ]; then
    cmake -S "$source" -B "$root/mbedtls-build" \
        -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" >&2
    cmake --build "$root/mbedtls-build" --parallel >&2
    cmake --install "$root/mbedtls-build" >&2
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
