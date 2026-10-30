#!/bin/bash
# Build and run the PC-only dashboard demo (fake data) inside WSL:  bash scripts/web_demo.sh [port]
# Then open http://127.0.0.1:<port> (login admin / demo-pass-123, a test value that exists only here).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${1:-18090}"
OUT="${TMPDIR:-/tmp}/aid_web_demo"
JPEG="$ROOT/docs/samples/p4b_snapshot_1.jpg"
[ -f "$JPEG" ] || JPEG="$(ls "$ROOT"/docs/samples/*.jpg 2>/dev/null | head -n 1)"
[ -n "$JPEG" ] || { echo "need a JPEG in docs/samples/ for the demo picture"; exit 1; }
g++ -std=c++11 -Wall -Wextra -I"$ROOT/src/system" -I"$ROOT/src/webserver" -I"$ROOT/src/events" \
    "$ROOT/tools/web_demo.cc" "$ROOT/src/system/config.cc" "$ROOT/src/system/json_util.cc" "$ROOT/src/system/log.cc" \
    "$ROOT/src/webserver/api.cc" "$ROOT/src/webserver/auth.cc" "$ROOT/src/webserver/http_server.cc" \
    "$ROOT/src/events/event_engine.cc" "$ROOT/src/events/event_store.cc" -pthread -o "$OUT"
exec "$OUT" "$ROOT/web" "$JPEG" "$PORT"
