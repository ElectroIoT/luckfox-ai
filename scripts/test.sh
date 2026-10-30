#!/bin/bash
# Run the PC-side unit tests with the host compiler (inside WSL). These do not touch the board.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${TMPDIR:-/tmp}/aid_test_core"
g++ -std=c++11 -Wall -Wextra -I"$ROOT/src/system" -I"$ROOT/src/webserver" \
    "$ROOT/tests/test_core.cc" "$ROOT/src/system/config.cc" "$ROOT/src/webserver/http_server.cc" \
    -pthread -o "$OUT"
"$OUT"
