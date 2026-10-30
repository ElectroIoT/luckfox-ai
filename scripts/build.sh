#!/bin/bash
# Cross-compile luckfox-ai for the Luckfox Pico (RV1106). Run inside WSL Ubuntu 22.04:
#   bash scripts/build.sh [clean]
# The build directory lives on the Linux filesystem (fast); the result is copied to ./build/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export LUCKFOX_SDK_PATH="${LUCKFOX_SDK_PATH:-$HOME/luckfox-pico}"
export LUCKFOX_RKMPI_PATH="${LUCKFOX_RKMPI_PATH:-$HOME/luckfox_pico_rkmpi_example}"
BUILD_DIR="${BUILD_DIR:-$HOME/build-luckfox-ai}"

[ -d "$LUCKFOX_SDK_PATH/tools/linux/toolchain" ] || { echo "SDK not found at $LUCKFOX_SDK_PATH"; exit 1; }
[ -d "$LUCKFOX_RKMPI_PATH/lib/uclibc" ] || { echo "rkmpi example not found at $LUCKFOX_RKMPI_PATH"; exit 1; }

if [ "${1:-}" = "clean" ]; then
    rm -rf "$BUILD_DIR" "$ROOT/build"
    echo "cleaned"
    exit 0
fi

mkdir -p "$BUILD_DIR" "$ROOT/build"
cd "$BUILD_DIR"
cmake "$ROOT" -DCMAKE_BUILD_TYPE=Release
make -j"$(nproc)"
cp -f aid-camera aid-core "$ROOT/build/"
file "$ROOT/build/aid-core" | cut -c1-200
ls -l "$ROOT/build/"
