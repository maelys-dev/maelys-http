#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root"
version=$(sed -n '1p' VERSION)
./scripts/package-release.sh "$version" >/dev/null

source_tar="dist/maelys-http-$version.tar"
archive="$source_tar.gz"
expanded=$(mktemp)
trap 'rm -f "$expanded"' EXIT HUP INT TERM
gzip -dc "$archive" >"$expanded"
cmp "$source_tar" "$expanded"

if command -v shasum >/dev/null 2>&1; then
    (cd dist && shasum -a 256 -c SHA256SUMS)
else
    (cd dist && sha256sum -c SHA256SUMS)
fi

test "$(wc -l <dist/SHA256SUMS | tr -d ' ')" = 2
grep -Eq "  maelys-http-$version[.]tar$" dist/SHA256SUMS
grep -Eq "  maelys-http-$version[.]tar[.]gz$" dist/SHA256SUMS
echo 'canonical tar and compressed release checksums verified'
