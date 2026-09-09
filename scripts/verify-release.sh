#!/bin/sh
# Replays this product's gates on the exact commit a tag names, before any
# byte is packaged, for one TARGET of the release matrix. The release
# workflow runs it on every target; a developer can run it by hand.
#
# TARGET is the socle's target name (linux-x86_64, linux-arm64, macos-arm64)
# and only selects what the host can do: the Mbed TLS integration needs the
# provider, which every target has, and the sanitizers are not re-run here
# because CI already gates the merge.
set -eu

target="${1:?TARGET required}"
case "$target" in
    linux-x86_64|linux-arm64|macos-arm64) ;;
    *) echo "verify-release: unknown target: $target" >&2; exit 64 ;;
esac

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
cd "$root"

system_dir=${SYSTEM_DIR:-../maelys-system}
test -d "$system_dir" || {
    echo "verify-release: no maelys-system checkout at $system_dir; run scripts/checkout-dependency.sh maelys-system" >&2
    exit 66
}

make clean
make check install-check check-system-pin SYSTEM_DIR="$system_dir"
make tls-integration REQUIRE_MBEDTLS=1 SYSTEM_DIR="$system_dir"
echo "verify-release: $target passed the source, install and TLS gates"
