#!/bin/sh
# Checks the archives scripts/package-release.sh has just written: the
# compressed archive expands to the canonical tar byte for byte, both
# digests are the ones SHA256SUMS records, and the manifest covers those two
# files and nothing else. It reads dist/ and never rebuilds it, so what it
# verifies is what the release will publish.
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root"
version=$(sed -n '1p' VERSION)

source_tar="dist/maelys-http-$version.tar"
archive="$source_tar.gz"
test -f "$source_tar" || { echo "test-source-package: no $source_tar; run scripts/package-release.sh" >&2; exit 66; }
test -f "$archive" || { echo "test-source-package: no $archive; run scripts/package-release.sh" >&2; exit 66; }

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
