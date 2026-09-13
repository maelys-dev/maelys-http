#!/bin/sh
set -eu

version="${1:?version required}"
case "$version" in
    *[!0-9A-Za-z.-]*|'') echo 'invalid version' >&2; exit 1 ;;
esac

# The extracted tree carries the same Makefile, which derives SYSTEM_DIR from
# MAELYS_DEPENDENCIES_DIR: the environment already says where the pinned
# checkouts are, so this script passes no path of its own.
test -n "${MAELYS_DEPENDENCIES_DIR:-}" || {
    echo 'test-release-archive: MAELYS_DEPENDENCIES_DIR is unset; the pinned checkouts live apart' >&2
    exit 64
}

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
archive="$root/dist/maelys-http-$version.tar.gz"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

tar -xzf "$archive" -C "$work"
source_dir="$work/maelys-http-$version"
test -f "$source_dir/Makefile"
test -f "$source_dir/VERSION"
test "$(sed -n '1p' "$source_dir/VERSION")" = "$version"
env -u CPPFLAGS make -C "$source_dir" clean check
echo 'packaged source archive: full check passed'
