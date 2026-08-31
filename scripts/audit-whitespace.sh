#!/bin/sh
set -eu

if command -v rg >/dev/null 2>&1; then
    findings=$(rg -n '[ 	]+$' --hidden \
        -g '!build/**' -g '!dist/**' -g '!.git/**' .) && status=0 || status=$?
else
    findings=$(git grep -n -I -E '[[:blank:]]+$' -- .) && status=0 || status=$?
fi

if [ "$status" -eq 0 ]; then
    printf '%s\n' "$findings"
    echo 'trailing whitespace found' >&2
    exit 1
else
    if [ "$status" -ne 1 ]; then
        echo 'whitespace audit could not scan the source tree' >&2
        exit "$status"
    fi
fi

echo 'whitespace audit: ok'
