#!/bin/bash
# Remove luckfox-ai from the board and bring the stock camera app back.
#   bash scripts/uninstall.sh [--purge]
# Without --purge the folder /root/aid (settings, program, model) stays on the board, only the
# boot script is removed. With --purge /root/aid is deleted as well. The firmware itself is not
# touched. After this the stock app is back at the next reboot (the script reboots the board).
set -euo pipefail

BOARD="${BOARD:-root@192.168.1.160}"
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=8"
ssh_b() { ssh $SSH_OPTS "$BOARD" "$@"; }

echo "stopping the service"
ssh_b '[ -x /etc/init.d/S99aid ] && /etc/init.d/S99aid stop || true'
ssh_b 'rm -f /etc/init.d/S99aid'
if [ "${1:-}" = "--purge" ]; then
    echo "deleting /root/aid"
    ssh_b 'rm -rf /root/aid'
fi
echo "restarting the board so that the stock camera app comes back"
ssh_b '/bin/busybox reboot' || true
echo "done"
