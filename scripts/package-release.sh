#!/bin/sh
# Builds everything the release publishes, for one TARGET of the socle's
# matrix, into dist/. This is the socle's package_command.
#
# usage: scripts/package-release.sh [TARGET]
#
# Two targets package, under names of their own. macos-arm64 hands over to
# scripts/package-macos-archive.sh, which writes the binary archive. Every
# other target, and a call without one, writes the source archive below:
# source has no target, and maelys-release.conf has it packaged once, on
# linux-x86_64, since gzip does not compress one tree to the same bytes on
# every machine.
#
# For the source archive, dist/ receives, in this order:
#   maelys-http-VERSION.tar        the canonical archive git writes
#   maelys-http-VERSION.tar.gz     the same tree compressed
#   both .sha256 files and SHA256SUMS
#   maelys-http-VERSION.spdx.json  the SBOM, which names the compressed
#                                  archive and records its digest
# The release attests the SBOM against the file the SBOM itself names, so
# the document is generated after the archive it describes and never before.
set -eu

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
cd "$root"

target="${1:-}"
case "$target" in
    ''|linux-x86_64|linux-arm64|macos-arm64) ;;
    *) echo "package-release: unknown target: $target" >&2; exit 64 ;;
esac
if test "$target" = macos-arm64; then
    exec ./scripts/package-macos-archive.sh
fi

version=$(sed -n '1p' VERSION)
case "$version" in
    *[!0-9A-Za-z.-]*|'') echo 'package-release: invalid version' >&2; exit 65 ;;
esac
system_version=$(sed -n '1p' dependencies/maelys-system.pin)
system_version=${system_version#v}

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

./scripts/test-source-package.sh
./scripts/generate-sbom.sh "$version" "$system_version" >/dev/null
./scripts/test-release-archive.sh "$version"

echo "$archive"
