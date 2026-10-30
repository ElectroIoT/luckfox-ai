#!/bin/sh
# Read-only hardware/firmware probe for the Luckfox Pico (Buildroot, busybox sh).
# Does NOT write, install or change anything. Status: UNTESTED on hardware.
# Usage on the board:  sh probe_hardware.sh | tee /tmp/probe.txt

sec() { printf '\n===== %s =====\n' "$1"; }
run() { printf '$ %s\n' "$*"; "$@" 2>&1; }

sec "SYSTEM"
run uname -a
run cat /proc/device-tree/model
run cat /etc/os-release
run cat /proc/version
run cat /proc/cmdline
run uptime

sec "CPU / MEMORY"
run cat /proc/cpuinfo
run head -n 6 /proc/meminfo
run free
run cat /proc/iomem

sec "STORAGE"
run cat /proc/mtd
run cat /proc/partitions
run df -h
run mount
ls /dev/mmcblk* 2>&1

sec "CAMERA / ISP (nodes are discovered, not assumed)"
ls -l /dev/video* /dev/media* /dev/v4l-subdev* 2>&1
for m in /sys/class/video4linux/*; do [ -e "$m/name" ] && echo "$m: $(cat $m/name)"; done
dmesg 2>/dev/null | grep -i -E "sc3336|mis5001|sensor|rkisp|mipi|csi|camera" | tail -n 40
ls /oem/usr/share/iqfiles /etc/iqfiles 2>&1

sec "NPU / RKNN"
ls -l /sys/kernel/debug/rknpu 2>&1
cat /sys/kernel/debug/rknpu/version 2>&1
cat /sys/kernel/debug/rknpu/load 2>&1
for f in $(find / -name 'librknn*' 2>/dev/null); do
  echo "$f"; strings "$f" 2>/dev/null | grep -i -E "librknn.*version|RKNN.*version" | head -n 3
done
ls /sys/class/devfreq 2>&1

sec "MEDIA / STOCK APP"
ps | grep -i -E "rkipc|rtsp|smart_door|rkmpi" | grep -v grep
ls -l /oem/usr/bin /usr/bin/rkipc 2>&1 | head -n 40
for f in $(find / -name 'librockit*' -o -name 'librkaiq*' -o -name 'librtsp*' 2>/dev/null); do echo "$f"; done

sec "NETWORK / USB"
run ip addr
run ip route
run cat /etc/hostname
ls /sys/class/net 2>&1
run lsusb
ls /sys/class/udc 2>&1

sec "GPIO / SERIAL / WATCHDOG / THERMAL"
ls /sys/class/gpio 2>&1
ls /dev/ttyS* 2>&1
ls /dev/watchdog* 2>&1
for t in /sys/class/thermal/thermal_zone*; do [ -e "$t/type" ] && echo "$t: $(cat $t/type) $(cat $t/temp 2>&1)"; done
ls /sys/class/rtc 2>&1

sec "MODULES / SERVICES"
run lsmod
ls /etc/init.d 2>&1

sec "DONE"
