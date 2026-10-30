#!/bin/bash
# Copy the built program, dashboard and AI model to the board. Run inside WSL after scripts/build.sh:
#   BOARD=root@192.168.1.160 bash scripts/deploy.sh
# Uses SSH key login; no password is stored in this repository. The config file with the access
# token and the dashboard password are NOT copied: they only exist on the board.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOARD="${BOARD:-root@192.168.1.160}"
DEST="${DEST:-/root/aid}"
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=8"
SDK="${LUCKFOX_SDK_PATH:-$HOME/luckfox-pico}"
RKMPI="${LUCKFOX_RKMPI_PATH:-$HOME/luckfox_pico_rkmpi_example}"
STRIP="$SDK/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-strip"

[ -f "$ROOT/build/aid-core" ] || { echo "run scripts/build.sh first"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
for f in aid-core aid-camera aid-supervisor; do
    cp "$ROOT/build/$f" "$TMP/$f"
    [ -x "$STRIP" ] && "$STRIP" "$TMP/$f" || true     # debug info is not needed on the board and wastes flash
done

ssh $SSH_OPTS "$BOARD" "mkdir -p $DEST $DEST/model"
scp -q $SSH_OPTS "$TMP/aid-core" "$TMP/aid-camera" "$TMP/aid-supervisor" "$ROOT/scripts/stop_stock_camera.sh" "$BOARD:$DEST/"

# dashboard files (replace the whole folder so deleted files disappear)
ssh $SSH_OPTS "$BOARD" "rm -rf $DEST/web"
scp -q -r $SSH_OPTS "$ROOT/web" "$BOARD:$DEST/"

# AI model: only copied when missing (it is 7 MB of flash writes). Force with DEPLOY_MODEL=1.
if [ "${DEPLOY_MODEL:-0}" = "1" ] || ! ssh $SSH_OPTS "$BOARD" "[ -f $DEST/model/yolov5.rknn ]"; then
    M="$RKMPI/example/luckfox_pico_rtsp_yolov5/model"
    [ -f "$M/yolov5.rknn" ] || { echo "model not found at $M"; exit 1; }
    scp -q $SSH_OPTS "$M/yolov5.rknn" "$M/coco_80_labels_list.txt" "$BOARD:$DEST/model/"
fi

ssh $SSH_OPTS "$BOARD" "chmod +x $DEST/aid-core $DEST/aid-camera $DEST/aid-supervisor $DEST/stop_stock_camera.sh; ls -l $DEST $DEST/model; du -sk $DEST | cut -f1 | sed 's/^/aid dir KB: /'"
