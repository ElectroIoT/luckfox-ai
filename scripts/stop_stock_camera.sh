#!/bin/sh
# Runs ON THE BOARD. Stops the stock camera app (rkipc) so our own program can use the camera.
# Same idea as Rockchip's /oem/usr/bin/RkLunch-stop.sh. A board reboot starts rkipc again.
killall rkipc 2>/dev/null
n=0
while ps | grep -q '[r]kipc -a'; do
    n=$((n + 1))
    if [ "$n" -gt 10 ]; then
        echo "rkipc is still running after 10 s" >&2
        exit 1
    fi
    sleep 1
done
# rkipc's RTSP socket (port 554) can stay open inside the USB-network udhcpc; only needed
# when our own RTSP server starts (Phase 4), so it is not touched here.
echo "rkipc stopped"
