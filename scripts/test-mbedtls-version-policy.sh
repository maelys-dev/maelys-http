#!/bin/sh
set -eu

cc=${CC:-cc}

compile() {
    policy_major=$1
    policy_minor=$2
    policy_patch=$3
    shift 3
    "$cc" -std=c11 -Wall -Wextra -Werror -I. -DMBEDTLS_VERSION_C=1 \
        -DMBEDTLS_VERSION_MAJOR="$policy_major" \
        -DMBEDTLS_VERSION_MINOR="$policy_minor" \
        -DMBEDTLS_VERSION_PATCH="$policy_patch" \
        "$@" -fsyntax-only tests/mbedtls_version_probe.c
}

reject() {
    reject_major=$1
    reject_minor=$2
    reject_patch=$3
    shift 3
    if compile "$reject_major" "$reject_minor" "$reject_patch" "$@" \
            >/dev/null 2>&1; then
        echo "unexpectedly accepted Mbed TLS $reject_major.$reject_minor.$reject_patch" >&2
        exit 1
    fi
}

compile 3 6 7
compile 3 7 0
compile 4 1 2
compile 4 2 0
reject 2 28 9
reject 3 5 9
reject 3 6 6
reject 4 0 0
reject 4 1 1
reject 5 0 0
reject 3 6 7 -UMBEDTLS_VERSION_C
compile 3 6 6 -DMAELYS_HTTP_MBEDTLS_ALLOW_BACKPORTED_SECURITY_FIXES=1
compile 4 1 1 -DMAELYS_HTTP_MBEDTLS_ALLOW_BACKPORTED_SECURITY_FIXES=1
reject 3 5 9 -DMAELYS_HTTP_MBEDTLS_ALLOW_BACKPORTED_SECURITY_FIXES=1
reject 4 0 9 -DMAELYS_HTTP_MBEDTLS_ALLOW_BACKPORTED_SECURITY_FIXES=1

echo 'Mbed TLS version policy: ok'
