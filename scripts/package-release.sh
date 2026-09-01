#!/bin/sh
set -eu

version="${1:?version required}"
case "$version" in
    *[!0-9A-Za-z.-]*|'') echo 'invalid version' >&2; exit 1 ;;
esac
mkdir -p dist
source_tar="dist/maelys-http-$version.tar"
archive="$source_tar.gz"
git archive --format=tar --prefix="maelys-http-$version/" HEAD >"$source_tar"
gzip -n -9 <"$source_tar" >"$archive"

checksum() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1"
    else
        sha256sum "$1"
    fi
}

(cd dist && checksum "$(basename "$source_tar")") >"$source_tar.sha256"
(cd dist && checksum "$(basename "$archive")") >"$archive.sha256"
cat "$source_tar.sha256" "$archive.sha256" >dist/SHA256SUMS
echo "$archive"
