#!/bin/sh
#
# Regression test runner for the WebSocket reassembly length arithmetic
# (see test_ws_reassembly_len.c). Self-contained: no libwebsockets needed, so
# it runs even in builds configured with --disable-websockets.
#
#   ./run_tests.sh            # build + run the safe (fixed) variant  -> exits 0
#   ./run_tests.sh vulnerable # build + run the pre-fix 32-bit variant -> exits 1
#                             #   (demonstrates the test detects the bug)
#
set -e

CC=${CC:-cc}
CFLAGS=${CFLAGS:--O2 -Wall -Wextra}
DIR=$(dirname "$0")

if [ "$1" = "vulnerable" ]; then
    echo "Building VULNERABLE (pre-fix) variant - this is expected to FAIL:"
    # shellcheck disable=SC2086
    $CC $CFLAGS -DVULNERABLE -o "$DIR/test_ws_reassembly_len_vuln" "$DIR/test_ws_reassembly_len.c"
    "$DIR/test_ws_reassembly_len_vuln"
else
    # shellcheck disable=SC2086
    $CC $CFLAGS -o "$DIR/test_ws_reassembly_len" "$DIR/test_ws_reassembly_len.c"
    "$DIR/test_ws_reassembly_len"
fi
