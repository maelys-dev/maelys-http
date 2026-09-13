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

# The pinned checkouts live apart from this repository, under the root the
# socle materialises and exports; the Makefile derives SYSTEM_DIR from it.
test -n "${MAELYS_DEPENDENCIES_DIR:-}" || {
    echo 'verify-release: MAELYS_DEPENDENCIES_DIR is unset; the pinned checkouts live apart' >&2
    echo '  eval "$(sh scripts/checkout-dependencies.sh "$PWD/../maelys-http-deps")"' >&2
    exit 64
}

# Before anything is built, including Mbed TLS, whose prefix is deliberately
# outside this tree and survives it.
make clean

case "$target" in
    linux-*) eval "$(sh scripts/build-pinned-mbedtls.sh)" ;;
esac

make check install-check check-system-pin
# REQUIRE_MBEDTLS=1: without it a provider pkg-config cannot see turns the
# target into a skip, and the release would report a success that proved
# nothing about TLS.
make tls-integration REQUIRE_MBEDTLS=1
echo "verify-release: $target passed the tag, source, install and TLS gates"
