#!/bin/sh
# Builds the binary archive of macos-arm64 into dist/, on the model of
# maelys-system's: what `make install-mbedtls` installs under /usr/local —
# the three static archives, the headers and the pkg-config files — and no
# shared library. scripts/package-release.sh macos-arm64 runs it.
#
# Mbed TLS is not in the archive: the consumer links its own, and the TLS
# provider embeds Mbed TLS structures whose size depends on the version and
# on the configuration. Homebrew's 4.2.0 enables threading and DTLS-SRTP,
# and its mbedtls_ssl_context is 848 bytes where the upstream default is 584.
# The provider is therefore compiled against Homebrew's Mbed TLS, the one a
# macOS consumer links and the one the bottles use, and only when its
# version is the one dependencies/mbedtls-4.pin names. Its pkg-config file
# accepts that minor line only, and share/maelys-http/build-info records
# the version, the configuration and the structure sizes.
#
# dist/ receives:
#   maelys-http-VERSION-macos-arm64.tar.gz         the archive
#   maelys-http-VERSION-macos-arm64.tar.gz.sha256
#   maelys-http-VERSION-macos-arm64.spdx.json      its SBOM
set -eu

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
cd "$root"
target=macos-arm64

test "$(uname -s):$(uname -m)" = Darwin:arm64 || {
    echo "package-macos-archive: $target is built on macOS arm64 only" >&2
    exit 65
}
test -n "${MAELYS_DEPENDENCIES_DIR:-}" || {
    echo 'package-macos-archive: MAELYS_DEPENDENCIES_DIR is unset; the pinned checkouts live apart' >&2
    echo '  eval "$(sh scripts/checkout-dependencies.sh "$PWD/../maelys-http-deps")"' >&2
    exit 64
}
system_dir="$MAELYS_DEPENDENCIES_DIR/maelys-system"

version=$(sed -n '1p' VERSION)
case "$version" in
    *[!0-9A-Za-z.-]*|'') echo 'package-macos-archive: invalid version' >&2; exit 65 ;;
esac
system_version=$(sed -n '1p' dependencies/maelys-system.pin)
system_version=${system_version#v}
system_commit=$(sed -n '2p' dependencies/maelys-system.pin)
mbedtls_version=$(sed -n '1p' dependencies/mbedtls-4.pin)
mbedtls_version=${mbedtls_version#v}
case "$mbedtls_version" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) echo 'package-macos-archive: invalid version in dependencies/mbedtls-4.pin' >&2; exit 65 ;;
esac
mbedtls_major=${mbedtls_version%%.*}
mbedtls_minor=${mbedtls_version#*.}
mbedtls_minor=${mbedtls_minor%%.*}
mbedtls_below="$mbedtls_major.$((mbedtls_minor + 1))"

# The Mbed TLS pkg-config finds must be Homebrew's, at the pinned version.
brew_prefix=$(brew --prefix)
for module in mbedtls mbedx509 mbedcrypto; do
    found=$(pkg-config --modversion "$module") || {
        echo "package-macos-archive: pkg-config finds no $module; brew install mbedtls" >&2
        exit 69
    }
    test "$found" = "$mbedtls_version" || {
        echo "package-macos-archive: $module is $found, dependencies/mbedtls-4.pin names $mbedtls_version" >&2
        exit 69
    }
    case "$(pkg-config --variable=prefix "$module")" in
        "$brew_prefix"/*) ;;
        *) echo "package-macos-archive: $module does not come from Homebrew under $brew_prefix" >&2; exit 69 ;;
    esac
done
mbedtls_libdir=$(pkg-config --variable=libdir mbedtls)

work=$(mktemp -d "${TMPDIR:-/tmp}/maelys-http-package.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

# What the provider depends on in that Mbed TLS: the options Homebrew turns
# on beyond the upstream defaults, and the sizes of the structures it embeds.
cat >"$work/probe.c" <<'EOF'
#include <stdio.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
int main(void) {
    printf("mbedtls-config");
#if defined(MBEDTLS_THREADING_C)
    printf(" MBEDTLS_THREADING_C");
#endif
#if defined(MBEDTLS_THREADING_PTHREAD)
    printf(" MBEDTLS_THREADING_PTHREAD");
#endif
#if defined(MBEDTLS_SSL_DTLS_SRTP)
    printf(" MBEDTLS_SSL_DTLS_SRTP");
#endif
    printf("\nmbedtls-layout ssl_config=%zu x509_crt=%zu ssl_context=%zu\n",
           sizeof(mbedtls_ssl_config), sizeof(mbedtls_x509_crt),
           sizeof(mbedtls_ssl_context));
    return 0;
}
EOF
# shellcheck disable=SC2046
${CC:-cc} $(pkg-config --cflags mbedtls) "$work/probe.c" -o "$work/probe"
mbedtls_facts=$("$work/probe")

make clean
make check-system-pin
stage="$work/stage"
make install-mbedtls DESTDIR="$stage" PREFIX=/usr/local REQUIRE_MBEDTLS=1 \
    MBEDTLS_PKGCONFIG_MIN_VERSION="$mbedtls_version" \
    MBEDTLS_PKGCONFIG_BELOW="$mbedtls_below"

install -d "$stage/usr/local/share/maelys-http"
cat >"$stage/usr/local/share/maelys-http/build-info" <<EOF
maelys-http $version
target $target
commit $(git rev-parse HEAD)
maelys-system $system_version $system_commit
mbedtls $mbedtls_version homebrew
$mbedtls_facts
EOF

mkdir -p dist
name="maelys-http-$version-$target"
archive="dist/$name.tar.gz"
COPYFILE_DISABLE=1 tar -czf "$archive" --uid 0 --gid 0 --uname root --gname wheel \
    -C "$stage" .
(cd dist && shasum -a 256 "$name.tar.gz") >"$archive.sha256"

listing="$work/listing"
tar -tzf "$archive" >"$listing"
for file in \
    lib/libmaelys_http.a lib/libmaelys_http_client.a lib/libmaelys_http_tls_mbedtls.a \
    lib/pkgconfig/maelys-http.pc lib/pkgconfig/maelys-http-client.pc \
    lib/pkgconfig/maelys-http-tls-mbedtls.pc \
    include/maelys/http.h include/maelys/http_client.h include/maelys/http_tls.h \
    include/maelys/http_tls_modules.h include/maelys/http_transports.h \
    share/maelys-http/build-info; do
    grep -qx "./usr/local/$file" "$listing" || {
        echo "package-macos-archive: $file is missing from $archive" >&2
        exit 70
    }
done
if grep -Eq '\.(dylib|so)$|/libmaelys_sys\.a$|/libmbed|/libtfpsacrypto' "$listing"; then
    echo "package-macos-archive: $archive carries a shared library or another product's archive" >&2
    exit 70
fi

# A consumer of the archive: maelys-system installed from its pinned
# checkout, the archive extracted beside it, and the TLS integration client
# linked statically from what the pkg-config files name, which proves them
# complete. Its only dynamic dependency must be libSystem.
consumer="$work/consumer"
mkdir -p "$consumer"
make -C "$system_dir" install DESTDIR="$consumer" PREFIX=/usr/local >/dev/null
tar -xzf "$archive" -C "$consumer"
consumer_pc="$consumer/usr/local/lib/pkgconfig"
PKG_CONFIG_PATH="$consumer_pc${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
    pkg-config --exists --print-errors maelys-http-tls-mbedtls maelys-http-client
archives=
for library in $(PKG_CONFIG_PATH="$consumer_pc${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
        pkg-config --static --libs-only-l maelys-http-tls-mbedtls maelys-http-client); do
    library=${library#-l}
    if test -f "$consumer/usr/local/lib/lib$library.a"; then
        archives="$archives $consumer/usr/local/lib/lib$library.a"
    elif test -f "$mbedtls_libdir/lib$library.a"; then
        archives="$archives $mbedtls_libdir/lib$library.a"
    else
        echo "package-macos-archive: no static lib$library.a for the consumer" >&2
        exit 70
    fi
done
client="$work/tls_integration_client"
# shellcheck disable=SC2046,SC2086
${CC:-cc} -std=c11 -Wall -Wextra -Werror -I"$consumer/usr/local/include" \
    $(pkg-config --cflags mbedtls) tests/tls_integration_client.c \
    $archives -pthread -o "$client"
otool -L "$client" | sed '1d' >"$work/dynamic"
if grep -v '^[[:space:]]*/usr/lib/libSystem\.B\.dylib ' "$work/dynamic" | grep -q .; then
    cat "$work/dynamic" >&2
    echo 'package-macos-archive: the consumer links more than libSystem' >&2
    exit 70
fi
MAELYS_HTTP_TLS_CLIENT="$client" ./scripts/test-tls-integration.sh

./scripts/generate-sbom.sh "$version" "$system_version" "$target" "$mbedtls_version" >/dev/null
echo "$archive"
