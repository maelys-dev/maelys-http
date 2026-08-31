#!/bin/sh
set -eu

bad="$({ nm -g build/libmaelys_http.a; nm -g build/libmaelys_http_client.a; } |
    awk '
        NF >= 3 {
            type = toupper($(NF - 1));
            name = $NF;
            sub(/^_/, "", name);
            if (type ~ /^[TDBRSC]$/ && name !~ /^maelys_http_/) print name;
        }
    ' | sort -u)"

if [ -n "$bad" ]; then
    echo "unexpected exported symbols:" >&2
    echo "$bad" >&2
    exit 1
fi
echo 'symbol audit: ok'
