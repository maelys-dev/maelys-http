#!/bin/sh
# Copies VERSION into the public header, which materialises it a second time
# as three macros a consumer compiles against. `cut` runs this between writing
# VERSION and the bump commit, so the two never travel apart: `make
# check-version` compares them, and a release whose header still named the
# previous patch would fail its own pull request.
#
# The macros are rewritten in place rather than regenerated, so everything
# around them — the ABI number above them, which moves on its own rules — is
# left exactly as it is.
set -eu

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
cd "$root"
header=include/maelys/http.h

version=$(sed -n '1p' VERSION)
major=${version%%.*}
rest=${version#*.}
minor=${rest%%.*}
patch=${rest#*.}
for part in "$major" "$minor" "$patch"; do
    case "$part" in
        ''|*[!0-9]*) echo "bump-version: VERSION is not X.Y.Z: $version" >&2; exit 65 ;;
    esac
done

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT HUP INT TERM
sed -e "s/^#define MAELYS_HTTP_VERSION_MAJOR [0-9][0-9]*u\$/#define MAELYS_HTTP_VERSION_MAJOR ${major}u/" \
    -e "s/^#define MAELYS_HTTP_VERSION_MINOR [0-9][0-9]*u\$/#define MAELYS_HTTP_VERSION_MINOR ${minor}u/" \
    -e "s/^#define MAELYS_HTTP_VERSION_PATCH [0-9][0-9]*u\$/#define MAELYS_HTTP_VERSION_PATCH ${patch}u/" \
    "$header" >"$tmp"
cat "$tmp" >"$header"

# The sed above replaces nothing if a macro is ever renamed or reformatted,
# and would then leave the header quietly holding the previous version.
test "$(sed -n 's/^#define MAELYS_HTTP_VERSION_MAJOR \([0-9][0-9]*\)u$/\1/p' "$header").$(sed -n 's/^#define MAELYS_HTTP_VERSION_MINOR \([0-9][0-9]*\)u$/\1/p' "$header").$(sed -n 's/^#define MAELYS_HTTP_VERSION_PATCH \([0-9][0-9]*\)u$/\1/p' "$header")" = "$version" || {
    echo "bump-version: $header does not carry $version after the rewrite" >&2
    exit 65
}
echo "bump-version: $header carries $version"
