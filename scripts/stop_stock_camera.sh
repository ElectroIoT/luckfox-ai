#!/bin/sh
# Runs ON THE BOARD. Stops the stock camera app (rkipc) so our own program can use the camera.
# Same idea as Rockchip's /oem/usr/bin/RkLunch-stop.sh. A board reboot starts rkipc again.
killall rkipc 2>/dev/null
n=0
while ps | grep -q '[r]kipc -a'; do
    n=$((n + 1))
    if [ "$n" -gt 30 ]; then
        echo "rkipc is still running after 30 s" >&2
        exit 1
    fi
    sleep 1
done
# rkipc's RTSP listening socket (port 554) is inherited by the USB-network DHCP client
# (udhcpc -i usb0) and stays open after rkipc exits, which would block our own RTSP server.
# Rockchip's RkLunch-stop.sh kills udhcpc for the same reason. The USB link keeps its fixed
# address; only its DHCP client goes away until the next boot.
killall udhcpc 2>/dev/null
echo "rkipc stopped"
