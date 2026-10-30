#!/bin/bash
# Copy the built tools to the board. Run inside WSL after scripts/build.sh:
#   BOARD=root@192.168.1.160 bash scripts/deploy.sh
# Uses SSH key login; no password is stored in this repository.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOARD="${BOARD:-root@192.168.1.160}"
DEST="${DEST:-/root/aid}"
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=8"

[ -f "$ROOT/build/aid-camera" ] || { echo "run scripts/build.sh first"; exit 1; }

ssh $SSH_OPTS "$BOARD" "mkdir -p $DEST"
scp -q $SSH_OPTS "$ROOT/build/aid-camera" "$ROOT/build/aid-core" "$ROOT/scripts/stop_stock_camera.sh" "$BOARD:$DEST/"
ssh $SSH_OPTS "$BOARD" "chmod +x $DEST/aid-camera $DEST/aid-core $DEST/stop_stock_camera.sh; ls -l $DEST"
# The config file with the HTTP token is NOT copied here: it only exists on the board (see config/aid.conf.example).
