#!/bin/bash
# Back up the board's luckfox-ai settings (and optionally low-level flash partitions) to the PC.
#   bash scripts/backup.sh [--firmware]
# Run inside WSL. Backups go to ./backups/<timestamp>/ (git-ignored). They contain secrets
# (access token, password hash): keep them private.
#
# --firmware also reads the small raw flash partitions (env, idblock, uboot, boot, oem, userdata;
# about 46 MB). This puts read load on the board's SPI NAND, which showed controller timeouts
# during development (docs/streaming.md), so it is off by default. The rootfs (210 MB) is not
# included: it can be rebuilt by flashing the official image.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOARD="${BOARD:-root@192.168.1.160}"
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=8"
OUT="$ROOT/backups/$(date +%Y%m%d-%H%M%S)"
umask 077
mkdir -p "$OUT"

echo "backing up to $OUT"
ssh $SSH_OPTS "$BOARD" 'echo "kernel: $(uname -a)"; echo; cat /proc/mtd; echo; ls -l /etc/init.d; echo; df -k' > "$OUT/board-info.txt" 2>&1 || true
for f in aid.conf auth.json; do
    scp -q $SSH_OPTS "$BOARD:/root/aid/$f" "$OUT/$f" 2>/dev/null && echo "  saved $f" || echo "  (no $f on the board)"
done
if ssh $SSH_OPTS "$BOARD" '[ -f /tmp/aid-data/events.jsonl ]' 2>/dev/null; then
    scp -q $SSH_OPTS "$BOARD:/tmp/aid-data/events.jsonl" "$OUT/events.jsonl" && echo "  saved events.jsonl"
fi
scp -q $SSH_OPTS "$BOARD:/etc/init.d/S99aid" "$OUT/S99aid" 2>/dev/null && echo "  saved S99aid" || true

if [ "${1:-}" = "--firmware" ]; then
    echo "reading raw flash partitions (this puts load on the flash; stop with Ctrl+C if the board misbehaves)"
    for n in 0 1 2 3 4 5; do
        name=$(ssh $SSH_OPTS "$BOARD" "awk -F'\"' '/^mtd$n:/ {print \$2}' /proc/mtd")
        echo "  mtd$n ($name)"
        ssh $SSH_OPTS "$BOARD" "cat /dev/mtdblock$n" > "$OUT/mtd$n-$name.bin"
        ls -l "$OUT/mtd$n-$name.bin" | awk '{print "    " $5 " bytes"}'
    done
fi
echo "done: $OUT"
