#!/bin/bash
# Run the PC-side unit tests with the host compiler (inside WSL). These do not touch the board.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${TMPDIR:-/tmp}/aid_tests"
mkdir -p "$OUT"
INC=(-I"$ROOT/src/system" -I"$ROOT/src/webserver" -I"$ROOT/src/events" -I"$ROOT/src/mqtt" -I"$ROOT/src/gpio" -I"$ROOT/src/ai" -I"$ROOT/src/camera")
SYS=("$ROOT/src/system/config.cc" "$ROOT/src/system/json_util.cc" "$ROOT/src/system/log.cc")

# Each test binary is built from the sources it needs; a test whose sources do not exist yet is skipped.
build_run() {
    local name="$1"; shift
    local f
    for f in "$@"; do [ -f "$f" ] || { echo "[skip] $name (missing ${f#$ROOT/})"; return 0; }; done
    g++ -std=c++11 -Wall -Wextra "${INC[@]}" "$@" -pthread -o "$OUT/$name"
    echo "--- $name"
    "$OUT/$name"
}

build_run test_core   "$ROOT/tests/test_core.cc" "${SYS[@]}" "$ROOT/src/webserver/http_server.cc" "$ROOT/src/system/sysinfo.cc"
build_run test_events "$ROOT/tests/test_events.cc" "${SYS[@]}" "$ROOT/src/events/event_engine.cc" "$ROOT/src/events/event_store.cc"
build_run test_auth   "$ROOT/tests/test_auth.cc" "${SYS[@]}" "$ROOT/src/webserver/auth.cc"
build_run test_mqtt   "$ROOT/tests/test_mqtt.cc" "${SYS[@]}" "$ROOT/src/mqtt/mqtt_client.cc" "$ROOT/src/mqtt/ha_discovery.cc"
build_run test_gpio   "$ROOT/tests/test_gpio.cc" "${SYS[@]}" "$ROOT/src/gpio/gpio.cc"
build_run test_ai     "$ROOT/tests/test_ai.cc" "$ROOT/src/ai/preprocess.cc" "$ROOT/src/ai/motion.cc"
if command -v python3 >/dev/null 2>&1; then
    echo "--- test_telegram_bridge"
    python3 -m unittest "$ROOT/tests/test_telegram_bridge.py" 2>&1 | tail -4
else
    echo "[skip] test_telegram_bridge (python3 not found)"
fi
build_run test_api   "$ROOT/tests/test_api.cc" "${SYS[@]}" "$ROOT/src/webserver/api.cc" "$ROOT/src/webserver/auth.cc" "$ROOT/src/webserver/http_server.cc" "$ROOT/src/events/event_engine.cc" "$ROOT/src/events/event_store.cc"
