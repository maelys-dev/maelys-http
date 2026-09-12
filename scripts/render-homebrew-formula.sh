#!/bin/sh
# usage: scripts/render-homebrew-formula.sh TAG OUTPUT NAME
#
# Renders packaging/homebrew/NAME.rb.in for one published tag. Everything the
# formula states is read from the release itself and never typed: the source
# archive is downloaded, its digest is the one Homebrew will check, and the
# template, VERSION and the Maelys System pin all come from inside that same
# archive.
#
# Reading the tag's own bytes rather than this working tree is what makes the
# formula independent of where it is rendered. The socle's tap workflow runs
# this on macOS while the release built the archive on Linux, and gzip is not
# byte-identical between the two: a formula that hashed a locally rebuilt
# archive would name a digest no downloader ever computes. It also survives a
# workflow_dispatch replay, where the checkout is a branch and not the tag.
#
# The archive must therefore be published before this runs, which is exactly
# the order the socle uses: the tap job needs the release job.
set -eu

tag="${1:?TAG required}"
output="${2:?OUTPUT required}"
name="${3:?NAME required}"

case "$name" in
    *[!a-z0-9-]*|'') echo "render-homebrew-formula: NAME must be [a-z0-9-]: $name" >&2; exit 64 ;;
esac
case "$tag" in
    v*) version=${tag#v} ;;
    *) echo "render-homebrew-formula: TAG must be vX.Y.Z: $tag" >&2; exit 64 ;;
esac
case "$version" in
    *[!0-9A-Za-z.-]*|'') echo "render-homebrew-formula: invalid version in $tag" >&2; exit 64 ;;
esac

repository=${MAELYS_SOURCE_REPOSITORY:-maelys-dev/maelys-http}
url="https://github.com/$repository/releases/download/$tag/maelys-http-$version.tar.gz"

work=$(mktemp -d "${TMPDIR:-/tmp}/maelys-http-formula.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
curl -fsSL --retry 5 --retry-delay 3 -o "$work/source.tar.gz" "$url"

if command -v sha256sum >/dev/null 2>&1; then
    digest=$(sha256sum "$work/source.tar.gz" | awk '{print $1}')
else
    digest=$(shasum -a 256 "$work/source.tar.gz" | awk '{print $1}')
fi
case "$digest" in
    *[!0-9a-f]*|'') echo 'render-homebrew-formula: invalid archive digest' >&2; exit 65 ;;
esac
test "${#digest}" = 64

mkdir -p "$work/tag"
tar -xzf "$work/source.tar.gz" -C "$work/tag" --strip-components=1

# The archive must be the tag it is named after, or the formula would state a
# version the bytes do not carry.
test "$(sed -n '1p' "$work/tag/VERSION")" = "$version" || {
    echo "render-homebrew-formula: $tag carries VERSION $(sed -n '1p' "$work/tag/VERSION")" >&2
    exit 65
}

template="$work/tag/packaging/homebrew/$name.rb.in"
test -f "$template" || {
    echo "render-homebrew-formula: $tag carries no packaging/homebrew/$name.rb.in" >&2
    exit 66
}

system_version=$(sed -n '1p' "$work/tag/dependencies/maelys-system.pin")
system_version=${system_version#v}
case "$system_version" in
    *[!0-9A-Za-z.-]*|'') echo 'render-homebrew-formula: invalid maelys-system pin' >&2; exit 65 ;;
esac

mkdir -p "$(dirname "$output")"
sed -e "s|@URL@|$url|g" -e "s/@VERSION@/$version/g" -e "s/@SHA256@/$digest/g" \
    -e "s/@SYSTEM_VERSION@/$system_version/g" "$template" >"$output"
grep -q '@[A-Z_]*@' "$output" && {
    echo "render-homebrew-formula: unsubstituted placeholder in $output" >&2
    exit 65
}
printf '%s\n' "$output"
