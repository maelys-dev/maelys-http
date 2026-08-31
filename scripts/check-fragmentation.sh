#!/bin/sh
set -eu
test -x build/test_parser
./build/test_parser >/dev/null
echo 'fragmentation oracle: all request cut points and chunked response cuts passed'
