#!/bin/sh
set -eu

version=$(sed -n '1p' VERSION)
archive="dist/maelys-http-$version.tar.gz"
test -f "$archive" || ./scripts/package-release.sh "$version" >/dev/null
if command -v sha256sum >/dev/null 2>&1; then
    digest=$(sha256sum "$archive" | awk '{print $1}')
else
    digest=$(shasum -a 256 "$archive" | awk '{print $1}')
fi
mkdir -p dist/homebrew
sed -e "s/@VERSION@/$version/g" -e "s/@SHA256@/$digest/g" \
    packaging/homebrew/maelys-http.rb.in >dist/homebrew/maelys-http.rb
printf '%s\n' "rendered dist/homebrew/maelys-http.rb"
