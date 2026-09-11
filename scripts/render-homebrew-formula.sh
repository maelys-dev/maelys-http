#!/bin/sh
# usage: scripts/render-homebrew-formula.sh TAG OUTPUT NAME
#
# Renders packaging/homebrew/NAME.rb.in for one released tag. Everything the
# formula states about this release is substituted here and never typed: the
# version and the digest come from the release archive, and the Maelys System
# version from dependencies/maelys-system.pin of the same tag, so the formula
# cannot drift from the source it installs.
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

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
cd "$root"
template="packaging/homebrew/$name.rb.in"
test -f "$template" || { echo "render-homebrew-formula: no template $template" >&2; exit 66; }

# The checkout must be the tag the formula claims, or the two disagree.
test "$(sed -n '1p' VERSION)" = "$version" || {
    echo "render-homebrew-formula: VERSION is $(sed -n '1p' VERSION), not $version" >&2
    exit 65
}

archive="dist/maelys-http-$version.tar.gz"
test -f "$archive" || ./scripts/package-release.sh "$version" >/dev/null
if command -v sha256sum >/dev/null 2>&1; then
    digest=$(sha256sum "$archive" | awk '{print $1}')
else
    digest=$(shasum -a 256 "$archive" | awk '{print $1}')
fi
case "$digest" in
    *[!0-9a-f]*|'') echo 'render-homebrew-formula: invalid archive digest' >&2; exit 65 ;;
esac
test "${#digest}" = 64

system_version=$(sed -n '1p' dependencies/maelys-system.pin)
system_version=${system_version#v}
case "$system_version" in
    *[!0-9A-Za-z.-]*|'') echo 'render-homebrew-formula: invalid maelys-system pin' >&2; exit 65 ;;
esac

mkdir -p "$(dirname "$output")"
sed -e "s/@VERSION@/$version/g" -e "s/@SHA256@/$digest/g" \
    -e "s/@SYSTEM_VERSION@/$system_version/g" "$template" >"$output"
grep -q '@[A-Z_]*@' "$output" && {
    echo "render-homebrew-formula: unsubstituted placeholder in $output" >&2
    exit 65
}
printf '%s\n' "$output"
