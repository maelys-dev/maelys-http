#!/bin/sh
# Replays this product's gates on the exact commit a tag names, before any
# byte is packaged, for one TARGET of the release matrix. The socle's release
# workflow runs it as its verify_command; a developer can run it by hand.
#
# TARGET is the socle's target name (linux-x86_64, linux-arm64, macos-arm64)
# and selects where Mbed TLS comes from: the Linux distribution ships it
# below the floor providers/mbedtls_version_policy.h enforces, so a Linux
# target builds the commit dependencies/mbedtls.pin names, while macOS takes
# the one dependencies/packages asks brew for.
set -eu

target="${1:?TARGET required}"
case "$target" in
    linux-x86_64|linux-arm64|macos-arm64) ;;
    *) echo "verify-release: unknown target: $target" >&2; exit 64 ;;
esac

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
cd "$root"

# Who published this, before what was published is built. A tag no key of
# .github/release-allowed-signers signed stops here, with nothing packaged.
sh scripts/verify-tag-signature.sh

system_dir=${SYSTEM_DIR:-../maelys-system}
test -d "$system_dir" || {
    echo "verify-release: no maelys-system checkout at $system_dir; run scripts/checkout-dependency.sh maelys-system" >&2
    exit 66
}

case "$target" in
    linux-*)
        mbedtls_src=${MBEDTLS_DIR:-../mbedtls}
        test -d "$mbedtls_src" || {
            echo "verify-release: no Mbed TLS checkout at $mbedtls_src; run scripts/checkout-dependency.sh mbedtls" >&2
            exit 66
        }
        prefix=$(CDPATH='' cd -- "$(dirname "$mbedtls_src")" && pwd)/mbedtls-prefix
        if [ ! -f "$prefix/lib/pkgconfig/mbedtls.pc" ]; then
            cmake -S "$mbedtls_src" -B "$mbedtls_src-build" \
                -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF \
                -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix"
            cmake --build "$mbedtls_src-build" --parallel
            cmake --install "$mbedtls_src-build"
        fi
        test -f "$prefix/lib/pkgconfig/mbedtls.pc"
        PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
        export PKG_CONFIG_PATH
        ;;
esac

make clean
make check install-check check-system-pin SYSTEM_DIR="$system_dir"
# REQUIRE_MBEDTLS=1: without it a provider pkg-config cannot see turns the
# target into a skip, and the release would report a success that proved
# nothing about TLS.
make tls-integration REQUIRE_MBEDTLS=1 SYSTEM_DIR="$system_dir"
echo "verify-release: $target passed the tag, source, install and TLS gates"
