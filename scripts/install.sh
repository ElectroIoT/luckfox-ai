#!/bin/bash
# Install luckfox-ai on the board from the PC (inside WSL), including start at boot.
#   bash scripts/install.sh [--no-autostart]
# Needs: the project built (scripts/build.sh), SSH key login to the board, the board running the
# official Luckfox Buildroot firmware. What it does, in order:
#   1. backs up the current settings (scripts/backup.sh)
#   2. copies program, dashboard and AI model (scripts/deploy.sh)
#   3. creates /root/aid/aid.conf if missing (with a random access token)
#   4. sets a random dashboard password if none exists (stored privately in ~/.aid_admin_password)
#   5. installs the supervisor and the boot script /etc/init.d/S99aid (unless --no-autostart)
#   6. starts the service
# Undo with scripts/uninstall.sh. If something goes wrong at boot, create /root/aid/disabled over
# SSH (or in the serial console): the boot script then does nothing and the stock app returns.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOARD="${BOARD:-root@192.168.1.160}"
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=8"
AUTOSTART=1
[ "${1:-}" = "--no-autostart" ] && AUTOSTART=0
ssh_b() { ssh $SSH_OPTS "$BOARD" "$@"; }

echo "== checking the board"
MODEL=$(ssh_b 'cat /proc/device-tree/model' | tr -d '\0')
echo "   $MODEL"
case "$MODEL" in
    *Luckfox*) ;;
    *) echo "this does not look like a Luckfox Pico; aborting"; exit 1 ;;
esac
ssh_b 'dmesg | grep -c -i -E "rockchip-sfc.*timeout|ubifs error" || true' | grep -qx 0 || {
    echo "the board reports flash errors in its log: restart it first (/bin/busybox reboot), then run this again"; exit 1; }

echo "== backup"
bash "$ROOT/scripts/backup.sh" || true

echo "== copying files"
bash "$ROOT/scripts/deploy.sh" > /dev/null
echo "   done"

echo "== configuration"
if ssh_b '[ -f /root/aid/aid.conf ]'; then
    echo "   /root/aid/aid.conf exists, keeping it"
else
    umask 077
    TOKEN=$(openssl rand -hex 16)
    { grep -v '^http_token=' "$ROOT/config/aid.conf.example"; echo "http_token=$TOKEN"; } | ssh_b 'umask 077; cat > /root/aid/aid.conf'
    printf '%s' "$TOKEN" > "$HOME/.aid_token"
    echo "   created /root/aid/aid.conf (access token stored in ~/.aid_token on this PC)"
fi

echo "== dashboard password"
if ssh_b '[ -f /root/aid/auth.json ]'; then
    echo "   already set"
else
    umask 077
    openssl rand -base64 15 | tr -d '\n/+=' | cut -c1-16 > "$HOME/.aid_admin_password"
    cat "$HOME/.aid_admin_password" | ssh_b '/root/aid/aid-core --set-password' > /dev/null
    echo "   set. The password is stored in ~/.aid_admin_password on this PC (user: admin). Change it in the dashboard."
fi

if [ "$AUTOSTART" = "1" ]; then
    echo "== installing the boot script"
    scp -q $SSH_OPTS "$ROOT/services/S99aid" "$BOARD:/etc/init.d/S99aid"
    ssh_b 'chmod +x /etc/init.d/S99aid'
    echo "== starting"
    ssh_b '/etc/init.d/S99aid restart' || true
    sleep 5
    ssh_b '/etc/init.d/S99aid status' || true
else
    echo "== autostart not installed (--no-autostart). Start by hand: ssh $BOARD 'sh /root/aid/stop_stock_camera.sh; cd /root/aid && ./aid-core &'"
fi
echo
echo "Dashboard: http://$(echo "$BOARD" | cut -d@ -f2):8080/  (user admin)"
