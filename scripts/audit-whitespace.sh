#!/bin/sh
set -eu

if findings=$(rg -n '[ 	]+$' --hidden \
        -g '!build/**' -g '!dist/**' -g '!.git/**' .); then
    printf '%s\n' "$findings"
    echo 'trailing whitespace found' >&2
    exit 1
else
    status=$?
    if [ "$status" -ne 1 ]; then
        echo 'whitespace audit could not scan the source tree' >&2
        exit "$status"
    fi
fi

echo 'whitespace audit: ok'
