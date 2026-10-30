#!/bin/bash
# Put a settings backup (from scripts/backup.sh) back on the board and restart the service.
#   bash scripts/restore.sh backups/<timestamp>
# Raw flash partition images in a backup are NOT written back by this script: flashing needs the
# Rockchip tools and a person at the board (see docs/firmware.md).
set -euo pipefail

BOARD="${BOARD:-root@192.168.1.160}"
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=8"
DIR="${1:-}"
[ -n "$DIR" ] && [ -d "$DIR" ] || { echo "usage: bash scripts/restore.sh backups/<timestamp>"; exit 1; }

ssh $SSH_OPTS "$BOARD" "mkdir -p /root/aid"
for f in aid.conf auth.json; do
    if [ -f "$DIR/$f" ]; then
        scp -q $SSH_OPTS "$DIR/$f" "$BOARD:/root/aid/$f.restore"
        ssh $SSH_OPTS "$BOARD" "chmod 600 /root/aid/$f.restore && mv /root/aid/$f.restore /root/aid/$f"
        echo "restored $f"
    fi
done
ssh $SSH_OPTS "$BOARD" "[ -x /etc/init.d/S99aid ] && /etc/init.d/S99aid restart || echo 'service not installed: run scripts/install.sh'"
