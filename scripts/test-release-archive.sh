#!/bin/sh
set -eu

version="${1:?version required}"
system_dir="${2:?maelys-system checkout required}"
case "$version" in
    *[!0-9A-Za-z.-]*|'') echo 'invalid version' >&2; exit 1 ;;
esac

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
case "$system_dir" in
    /*) ;;
    *) system_dir="$root/$system_dir" ;;
esac
archive="$root/dist/maelys-http-$version.tar.gz"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

tar -xzf "$archive" -C "$work"
source_dir="$work/maelys-http-$version"
test -f "$source_dir/Makefile"
test -f "$source_dir/VERSION"
test "$(sed -n '1p' "$source_dir/VERSION")" = "$version"
env -u CPPFLAGS make -C "$source_dir" clean check SYSTEM_DIR="$system_dir"
echo 'packaged source archive: full check passed'
