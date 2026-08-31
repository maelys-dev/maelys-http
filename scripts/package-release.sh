#!/bin/sh
set -eu

version="${1:?version required}"
case "$version" in
    *[!0-9A-Za-z.-]*|'') echo 'invalid version' >&2; exit 1 ;;
esac
mkdir -p dist
archive="dist/maelys-http-$version.tar.gz"
git archive --format=tar --prefix="maelys-http-$version/" HEAD | \
    gzip -n -9 >"$archive"
if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$archive" >"$archive.sha256"
else
    sha256sum "$archive" >"$archive.sha256"
fi
echo "$archive"
