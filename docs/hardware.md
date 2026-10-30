# Phase 1 – Hardware & Firmware Compatibility Report

Luckfox Pico Pro – AI Vision Platform. Date: 2026-10-30.

**Evidence legend:** `[DOC]` read from official Luckfox wiki / GitHub / Rockchip model zoo on this date.
`[UNVERIFIED]` not confirmed by a source or by the device – must be checked on the real board.
Sections 1–7 come from documentation. Section 8 holds `[MEASURED]` results from the read-only probe run on the real board (2026-10-30).
No performance numbers are stated because none have been measured yet.

Sources: wiki.luckfox.com (Pico Pro Max, CSI Camera, GPIO, RKNN pages), github.com/LuckfoxTECH/luckfox-pico (README, SDK V1.4),
github.com/airockchip/rknn_model_zoo (README).

## 1. Board and SoC

| Item | Luckfox Pico **Pro** | Source |
|---|---|---|
| SoC | RV1106**G2** | [DOC] |
| CPU | single-core 32-bit Cortex-A7 @ up to 1.2 GHz, NEON + FPU | [DOC] |
| RAM | **128 MB** DDR3L | [DOC] |
| NPU | **0.5 TOPS**, INT4/INT8/INT16 (RKNPU2 family) | [DOC] |
| ISP | up to 5 MP @ 30 fps; HDR, 3A, LSC, 2D/3D NR, sharpen, dehaze, gamma | [DOC] |
| Camera | MIPI CSI **2-lane** | [DOC] |
| Ethernet | 10/100 Mbps, embedded PHY | [DOC] |
| USB | USB 2.0 Host/Device | [DOC] |
| GPIO | 26 pins, multiplexed as UART/SPI/I2C | [DOC] |
| Flash | 256 MB SPI NAND (default); can also boot from TF (microSD) | [DOC] |

The **Pro Max** wiki page covers two boards with identical layout: Pro (RV1106G2, 128 MB, 0.5 TOPS) and Max (RV1106G3, 256 MB, 1 TOPS).
They are visually the same. **Which one you hold must be confirmed on the device** (`/proc/meminfo` ≈ 128 MB vs 256 MB). The SDK `lunch`
menu has a single entry `RV1106_Luckfox_Pico_Pro_Max` for both. Exact PCB revision/silkscreen: [UNVERIFIED] – photo of the board needed.

## 2. Firmware / SDK

- Official SDK: `LuckfoxTECH/luckfox-pico`, current **V1.4** [DOC]. Kernel **5.10.160** [DOC]. Buildroot is the only listed rootfs for these boards [DOC].
- Build host: **Ubuntu 22.04 recommended** [DOC]. Your PC is Windows 11 → use WSL2 (Ubuntu 22.04) or a VM for the SDK and RKNN-Toolkit2 [my inference; WSL2 not yet tested].
- Toolchain: `arm-rockchip830-linux-uclibcgnueabihf` shipped in the SDK (uClibc – not glibc; binaries must be built with this toolchain) [DOC].
- Boot media for Pico Pro/Max: SPI NAND (and SD card per the SDK menu) [DOC].
- Existing firmware **on your board** (version, kernel string, build date): [UNVERIFIED] – read it with the probe script (section 7).
- The wiki documents RTSP at `rtsp://<ip>/live/0` for Pro/Max. **Correction (twice):** I first wrote that the flashed image (250607) has no `rkipc`. That was wrong: my probe listing was cut off by `head -n 40`, which hid `rkipc`. The board answers RTSP on port 554 (`Server: rtsp_demo`, the stock `rkipc` app), so the stock camera app IS running. See section 8.

## 3. Camera

- Officially documented sensors for Pro/Max: **SC3336 (3 MP)** and **MIS5001 (5 MP)**; Buildroot only [DOC].
- Your actual sensor, `/dev/video*`, `/dev/media*`, `/dev/v4l-subdev*` nodes: [UNVERIFIED]. Not assumed anywhere; Phase 3 detects at runtime.
- The ribbon cable must have its metal side facing the SoC side of the board; the connector latch breaks if forced [DOC].
- Native pipeline is Rockchip ISP + RKMPI (VI → VPSS → VENC). Exact RKMPI/MPP library versions: [UNVERIFIED] (obtainable from the SDK `media/` tree after cloning, or from the board).

## 4. NPU / AI models

Runtime on RV1106 is the mini runtime `librknnmrt` (seen in the SDK tree), **not** `librknnrt`. Do not copy an RK3588 runtime.
Wiki examples use RKNN-Toolkit2 **1.6.0** (Ubuntu 22.04, Python 3.10/3.11) [DOC]. The exact runtime version in the SDK/firmware is
[UNVERIFIED]; the converting toolkit must match it, so we will read it from `librknnmrt.so` first (Phase 5 gate).

RKNN Model Zoo marks RV1103/RV1106 as **"limited support"** [DOC]. Zoo entries listing RV1103|RV1106:

| Model | RV1106 listed | Notes |
|---|---|---|
| yolov5 (n, s_relu, s, m) | yes | best-trodden path; COCO 80 classes |
| yolov10 (n, s) | yes | |
| **yolo11 (n, s, m)** | yes | usable – README lists it for RV1106 |
| yolo26 (n, s, m) | yes | very new; treat as experimental |
| mobilenet (classification) | yes | classification, not detection |
| RetinaFace, LPRNet | LPRNet yes | not needed now |
| **yolov8** | **no** (not listed for RV1106) | don't plan on it |
| yolov6/7, yolox, ppyoloe, seg/pose variants | no | |
| MobileNet-SSD | not in the zoo README | would need a custom conversion; skip unless needed |

Performance: the zoo's FPS table has **no RV1106 column**, so there is no published figure. All speed/latency numbers will be measured on your board in Phase 5.
Preliminary model plan: start with `yolov5s_relu` or `yolov5n`, compare with `yolo11n`, INT8, 640×640 (smaller input if latency is too high) – to be decided by measurement. Class set is COCO, so person/car/motorcycle/bicycle/bus/truck/dog/cat are all available.

## 5. Storage, USB, Ethernet, GPIO, serial

- microSD: board can boot from TF [DOC]; SDK V1.4 notes improved SD compatibility [DOC]. Using it as a data card (mount, FS type, speed): [UNVERIFIED].
- 256 MB SPI NAND is too small and wears out – event snapshots/recordings should go to microSD, config to NAND.
- Ethernet 10/100 and USB 2.0 (RNDIS networking script `rndis.sh` exists in the stock firmware; it cannot run at the same time as UVC mode) [DOC].
- GPIO: sysfs numbering `pin = bank*32 + group*8 + X` (A=0…D=3) [DOC]. The pinout diagram is an image on the wiki; **no pin will be assigned** until I check it against the diagram and what the camera/Ethernet/USB use. Candidates (relay, buzzer, LED, PIR) are deferred to Phase 9+.
- Serial console: UART2 debug console is expected on this family [UNVERIFIED for this revision]. Baud rate to be confirmed (commonly 115200) – you need a USB-UART adapter for recovery.
- Watchdog, thermal sensor, RTC: [UNVERIFIED].

## 6. Risks / decisions

1. **RAM:** 128 MB is shared with ISP/VENC/NPU reserved memory, so usable RAM is well below 128 MB (value [UNVERIFIED]). Design budget decided after measuring `MemAvailable` with the stock app running.
2. **One A7 core:** web server, MQTT, SQLite, and JPEG/overlay must be light; encoding and inference are offloaded to VENC/NPU.
3. **SQLite vs JSON:** decide after measuring RAM; the cost is small, but NAND writes must avoid wear (store on microSD).
4. **Toolkit/runtime mismatch** is the main AI risk; resolved by reading the runtime version first.
5. **Windows host:** SDK build needs Linux (WSL2/VM). Flashing uses Rockchip tools (`upgrade_tool`/SocToolKit on Windows) in maskrom/loader mode – exact procedure to be confirmed from the wiki "Burn image" page in Phase 2.
6. **Safety:** no firmware write before a verified backup (Phase 2) and your explicit approval.

## 7. Next step – read-only device probe

`scripts/probe_hardware.sh` only reads information (no writes, no installs). Run it on the board over SSH/serial and send me the output; it settles every `[UNVERIFIED]` above.
Untested on hardware.

## 8. Probe results – `[MEASURED]` on the board, 2026-10-30

Firmware flashed to SPI NAND: Luckfox_Pico_Pro_Max_Flash_250607 (Buildroot). Probe run over SSH at `192.168.1.160`.

| Item | Result |
|---|---|
| Model string | `Luckfox Pico Pro Max` (same string for Pro and Max) |
| Which board | **Pro**: System RAM range is 0x00000000–0x07ffffff = 128 MB |
| OS | Buildroot 2023.02.6, kernel 5.10.160 (#22, 2025-06-11), armv7l, uClibc toolchain (gcc 8.3) |
| U-Boot | fwver 06/26/2025 |
| **Usable RAM** | `MemTotal` 55 612 kB; `MemAvailable` ≈ 37 MB two minutes after boot (load avg 9.3 while booting). CMA is 66 MB (`rk_dma_heap_cma=66M`) |
| Flash layout | SPI NAND, UBIFS: env 256K, idblock 256K, uboot 512K, boot 4M, oem 30M, userdata 10M, rootfs 210M |
| Free space | `/` 137 MB free of 181 MB; `/oem` 3.7 MB free; `/userdata` 2.2 MB free; `/tmp` is RAM (27 MB) |
| microSD | No `/dev/mmcblk*` (no card inserted at probe time; SDIO controller present at ffaa0000) |
| NPU | `rknpu` module loaded; `/dev/…`/debugfs `rknpu` entries absent (debugfs probably not mounted) |
| **RKNN runtime** | `/oem/usr/lib/librknnmrt.so` **version 1.6.0** (2de554906, 2024-01-17) |
| Media | `rockit`, `mpp_vcodec`, `rga3`, `rve`, `video_rkisp`, `video_rkcif` loaded; `librockit*.so`, `librkaiq.so` in `/oem/usr/lib`; `rk_mpi_*_test`, `mpi_enc_test`, `mpp_info_test` in `/oem/usr/bin`; `rkipc` is the stock camera app (the listing was truncated by `head -n 40`, so it looked absent) |
| ISP tuning files | `/etc/iqfiles`: sc3336, sc4336, mis5001 |
| **Camera sensor** | **NOT detected.** dmesg: `get remote sensor_sd failed`, `There is not terminal subdev`. Only csi2/dphy/isp sub-devices exist; no sensor sub-device |
| Video nodes | `/dev/video0…20`, `/dev/media0-1`, `/dev/v4l-subdev0-2` are ISP/CIF nodes, not a sensor |
| Network | eth0 DHCP → 192.168.1.160 (random MAC); usb0 up at 172.32.0.93/16 (Windows has no driver yet) |
| Console | kernel `console=ttyFIQ0` (UART debug); baud not shown in cmdline |
| GPIO | gpiochip0/32/64/96/128 (five banks) |
| Thermal | one zone `soc-thermal`: 51.1 °C (single reading right after boot) |
| Watchdog | **no `/dev/watchdog*`** found; RTC `rtc0` present |
| Services in `/etc/init.d` | sshd, **telnet**, usbdevice (**adb** over USB enabled), ntp, syslogd, klogd, python, `S99_auto_reboot`, luckfox config/overlay scripts |
| Hostname | `uname` shows `luckfox`; `/etc/hostname` printed as `luckfox pico` (check for a stray space) |

### What this changes

1. **RAM is the hard limit:** ≈ 37 MB available with nothing of ours running. Every component must be budgeted; SQLite is plausible, a large web stack is not.
2. **Camera is the blocker for Phase 3.** Either no camera is connected / the ribbon cable is wrong, or the sensor is not bound in the device tree. Next: power off, connect camera (metal contacts toward the SoC), power on, re-run `dmesg | grep -i -E "sc3336|sc4336|mis5001|i2c"`.
3. **AI models must match runtime 1.6.0:** use RKNN-Toolkit2 1.6.0 (wiki version) for conversion. Newer toolkit output may fail with "incompatible model version". yolov5 first; yolo11 is a risk until proven.
4. **Security work needed (Phase 9):** telnet and adb are enabled, root has a default-style password. Change the root password soon; disable telnet/adb before any production use.
5. **No hardware watchdog device found:** recovery must be checked (`/sys/class/watchdog`, kernel config) before promising auto-recovery. Also read `S99_auto_reboot` and `RkLunch.sh` to see what they do.
6. **Storage:** persistent app space on NAND is ≈ 137 MB (rootfs) and wear-limited; snapshots/DB belong on a microSD card (not yet tested).

### Still to check
`mpp_info_test` (MPP version), `cat /oem/usr/bin/RkLunch.sh`, `cat /etc/init.d/S99_auto_reboot`, `ls /sys/class/watchdog`, debugfs mount for NPU load, a microSD test with a real card, UART baud rate.

### Camera update – `[MEASURED]` 2026-10-30 (camera connected)
dmesg after connecting the camera: the `sc3336` driver probed at I2C address `4-0030`, read the chip ID (`Detected OV00cc41 sensor`; the `OV` is a generic prefix in the driver message) and `csi2-dphy0` reports `matches m00_b_sc3336`. The `mis5001` probe at `4-0031` failed with error -22, which is expected because no MIS5001 is attached. Early `get_remote_sensor ... failed` lines were logged before the sensor was bound.
Not yet confirmed: that the full ISP pipeline works (sub-device list after boot, an actual frame capture). Phase 3 verifies this.

### Second board check – `[MEASURED]` 2026-10-30 (raw output: `docs/checks.txt`)
- **Camera pipeline bound:** sub-devices are `rockchip-mipi-csi2`, `rockchip-csi2-dphy0`, `m00_b_sc3336 4-0030`, `rkisp-isp-subdev`, `rkcif-mipi-lvds` (`/dev/v4l-subdev0-4`). Frame capture still not tested.
- **MPP:** `mpp_info_test` reports version `16e796a4` (2024-02-20). RKMPI = `librockit` from the same SDK image (version string not yet read).
- **Boot flow:** `RkLunch.sh` runs DHCP on eth0 (`udhcpc -i eth0`), picks `rkipc-300w.ini` when the sc3336 module is loaded, then starts `rkipc`. `rkipc` does start: port 554 serves RTSP (see "Capture test" below). My earlier statement that it was absent was wrong (truncated listing).
- **`S99_auto_reboot`:** only acts if `/oem/rockchip_test/auto_reboot.sh` or files under `/data/cfg/rockchip_test/` exist (Rockchip factory test hooks). Not yet checked whether they exist; it must not reboot the board during our tests.
- **Tools on board:** `v4l2-ctl` and `python3` present; `ffmpeg` and `gst-launch-1.0` absent.
- **Watchdog:** `/proc/devices` lists the `watchdog` driver (major 248), but no `/dev/watchdog*` node and no `/proc/config.gz`; the `ls /sys/class/watchdog` printed nothing, which I read as "no watchdog device registered" (inference). A hardware watchdog would need a kernel/device-tree change; fallback is a software supervisor plus kernel panic-reboot. To be decided in Phase 9.

### PC build readiness – `[MEASURED]` 2026-10-30
WSL is **not installed**. Free disk: D: 443 GB, C: 705 GB. RAM 31.9 GB. Hypervisor present. The SDK needs Ubuntu 22.04: install WSL2 (`wsl --install -d Ubuntu-22.04`, admin PowerShell, then reboot).

### Capture test – `[MEASURED]` 2026-10-30
- `rk_mpi_vi_test -w 1920 -h 1080 -t 4 -c 0 -d 0 -m 1 -l 60 -o 1 --codec=8 -b 2048` crashed with a **segmentation fault** (no file written). The log showed `RKSockServer ... bind failed ... Address already in use`.
- Most likely cause (not yet proven): the stock `rkipc` is running and already owns the camera and the rockit socket. Rockchip's `RkLunch-stop.sh` does `killall rkipc` (plus `killall udhcpc` and stops init scripts) before custom camera programs run.
- `dmesg` shows `rkipc`-style encoder channels created ~8 s after boot (bps 512000, fps 30, gop 50; and a ~7.8 Mbps channel).
- Board ports from the PC: **22 (ssh), 23 (telnet), 554 (RTSP) open**; 80, 443, 8080, 8554 closed. RTSP `OPTIONS` on 554 → `200 OK`, `Server: rtsp_demo`. No web UI on port 80.
- `/data` is a symlink to `/userdata` (2.2 MB free). A temporary tmpfs was mounted over `/userdata` for the test (RAM only, undone by reboot or `umount`).

### Live view – `[TESTED ON HARDWARE]` 2026-10-30
Opening `rtsp://192.168.1.160/live/0` in VLC on the Windows PC shows live video from the SC3336 camera, served by the stock `rkipc` app (hardware ISP + encoder). This proves the sensor, ISP and RTSP path work end to end on the real board. Resolution, frame rate, latency and CPU/RAM use during streaming are **not yet measured**.
Design consequence for Phases 3–4: the SDK includes `rkipc` source, so we can either extend it or write our own capture/stream app. Our own app must stop `rkipc` first (only one program can own the camera).

### SSH key login and stock-app baseline – `[MEASURED]` 2026-10-30
- Key login from WSL (`~/.ssh/id_ed25519`, no passphrase) to `root@192.168.1.160` works. It failed at first because `/root` on the board was owned by uid 1005; sshd log: `Authentication refused: bad ownership or modes for directory /root`. Fix applied by the user: `chown root:root /root` (undo: `chown 1005:1005 /root`). Not yet checked that it survives a reboot.
- Stock app: `rkipc -a /oem/usr/share/iqfiles` (PID 666). Its config comes from `/oem/usr/share/rkipc-300w.ini` (via `/tmp/rkipc-factory-config.ini`).
- Stock stream settings (from the ini, not from a stream capture): main stream `video.0` 2304x1296 H.265 CBR max 2048 kbps, GOP 50; sub stream `video.1` 704x576 H.265 CBR 512 kbps; JPEG snapshots 1920x1080; AIQ (3A) on, auto exposure on, IVS on, NPU off, RTSP on, RTMP off, audio off.
- Single idle samples (no viewer): `rkipc` ≈ 7 % CPU, system ≈ 80 % idle, `MemAvailable` ≈ 29 MB, **`CmaFree` = 0 of 67 MB** (rkipc holds all the media memory), SoC temperature 62.4 °C, load average ≈ 10 for two hours while the CPU was mostly idle (probably blocked kernel threads; cause unverified).
- Implication: our own camera/NPU app cannot run alongside `rkipc` (CMA is exhausted, one program per camera). Either stop `rkipc`, or extend it. Not measured yet: CPU/RAM/fps while a client is streaming.

### Stock stream under load – `[MEASURED]` 2026-10-30
Setup: one VLC client on the Windows PC (192.168.1.115) playing `rtsp://192.168.1.160/live/0`; board sampled over SSH every 3 s for 18 s (`top`, `/proc/meminfo`, thermal zone, `/proc/net/dev`). Small sample, one run, one client.

| Metric | Result |
|---|---|
| RTSP client | 1 established connection on :554 |
| `rkipc` CPU | 5–13 % (typical ≈ 8 %) |
| System idle | 66–80 % |
| `MemAvailable` | 28 MB (constant) |
| SoC temperature | 63.6–65.4 °C |
| eth0 transmit | 4 034 758 bytes in ~18 s ≈ **1.8 Mbit/s** (includes the small SSH traffic; consistent with the 2 Mbit/s CBR limit in the config) |

Not measured: frame rate, picture latency, resolution actually delivered, NPU load (NPU is off in this app), behaviour with several clients, long-run stability.
Script: `scratchpad/board_sample.sh` (not kept in the repo yet).

### First NPU run: Luckfox `luckfox_pico_rtsp_yolov5` example – `[TESTED ON HARDWARE]` 2026-10-30
Built on the PC (WSL, SDK uClibc toolchain, `example/luckfox_pico_rtsp_yolov5`), copied to `/root/luckfox_pico_rtsp_yolov5_demo` on the board (10 MB on flash), started with `LD_LIBRARY_PATH=/oem/usr/lib` (the binary's rpath `$ORIGIN/lib` has no lib folder; all needed libs are in `/oem/usr/lib`).
Findings:
- The demo's first step `system("RkLunch-stop.sh")` fails (`not found`, the script is not in PATH), so `rkipc` keeps the camera. Stop it by hand: `killall rkipc`.
- Port 554 stays busy after `rkipc` stops: its RTSP socket was inherited by `udhcpc -i usb0` (PID 2088). `killall udhcpc` frees it (this is what the stock `RkLunch-stop.sh` does). After that our own RTSP server started on 554, path `/live/0`.
- Camera + ISP + hardware encoder + NPU inference all ran: the log prints COCO detections (it reported `traffic light` repeatedly, probably false positives; not evaluated).
- Example design (read from `main.cc`): 720x480 frames; NV12→BGR, letterbox resize to 640x640 and drawing done on the CPU with OpenCV-mobile; inference on every frame; single thread; no inference interval, no timing output.
- Single 24 s measurement, **no RTSP client connected**: process %CPU 54 % in `top` (snapshot), system 63 % user, idle 36–72 % across samples, `MemAvailable` ≈ 29 MB, process RSS 11.2 MB (VSZ 59 MB, 13 threads), SoC temperature rose **67.8 → 70.1 °C**. NPU load could not be read (`/sys/kernel/debug/rknpu` is absent even with debugfs mounted). My first sampling script printed %VSZ (107 %) as "CPU": that column was wrong and is ignored.
- **Not measured:** frame rate, inference latency (the example prints neither), picture quality, detection accuracy, behaviour with a client connected, thermal limit of the chip.
- A board reboot restores the stock app. The `/root` ownership fix and the demo folder remain on flash.

### With a viewer connected, then reboot – `[MEASURED]` 2026-10-30
- VLC on the PC watched the demo stream (one established RTSP connection) and showed the video **with detection boxes** (user confirmed).
- 24 s, viewer connected: demo %CPU 8–54 % (noisy single `top` snapshots; sampling adds a little load), system idle 18–63 %, `MemAvailable` 29–30 MB, SoC temperature 68.4 → 71.9 °C (rising), stream ≈ 615 kbit/s (720x480 H.264, default settings of the example). Frame rate, latency and NPU load still not measured.
- Board reboot (≈ 20 s to SSH): key login still works, `/root` is still owned by `root` (the chown fix survives reboot), stock `rkipc` is back and serves RTSP on :554, same IP 192.168.1.160, the demo folder on flash is kept.

### Phase 3 camera service and a flash incident – see `docs/camera.md`
- Our own `aid-camera` tool ran on the board: 30.02 fps measured at the camera input (1920x1080), JPEG snapshot via the hardware encoder in 4.05 ms (one run each).
- **Incident `[MEASURED]`:** after a burst of 8 camera open/close cycles the root filesystem went read-only (SPI flash controller DMA timeouts, `error -5` reads, UBIFS read-only). A restart restored it (flash error count 0 afterwards). Cause unknown; log saved in `docs/diag_dmesg_flash_error_2026-10-30.txt`. `sysrq` is not available in this kernel; `/bin/busybox reboot` works when applet symlinks are unreadable.

## 9. Final software on the board `[MEASURED]` 2026-10-30
(`aid-core` 1.0.0-rc1 + supervisor; numbers from the program's own `status.json` and `/proc`; single runs, one scene, one viewer.)
- Encoder 28.5 to 30 fps at 1920x1080, about 2 Mbit/s, capture-to-encoded latency 44.6 ms, key frame every 60 frames (counter verified: 9 in 504 frames after the counter fix).
- NPU inference 88.9 ms average (600+ runs), picture preparation 34 to 39 ms, post-processing about 2 ms.
- CPU: aid-core about 11.4 % of the single core in steady state (about 25 % in the first minute); the `/proc/stat` whole-system figure reads 5 to 7 % and is not trusted (it under-counts bursty load on this kernel: it came out lower than aid-core's own process time). RAM: aid-core RSS 8.6 MB, free about 28 MB (4 MB less after `mlockall`). SoC 62 to 66 C.
- Cold boot to dashboard answering: 26 s (reboot command to `/health` OK). Crash (`kill -9`) to dashboard back: 12 s.
- Snapshot: 7 to 10 ms inside the program, 11 ms over the LAN (70 KB).
- **9-minute soak** (18 samples, 30 s apart, final software): process uptime 20 s to 531 s, 15 208 frames, 1 034 detection runs; encoder 26.5 to 29.0 fps; inference 88.8 to 89.0 ms (no drift); aid-core RSS constant at 8.68 MB; free RAM constant at 28.0 MB; aid-core CPU 10.6 to 12.0 %; SoC temperature 63.6 to 69.6 C (rising slowly, no plateau seen); 16 threads, 71 open files; flash errors 0; root filesystem writable.
- **Longer run** (same software, 5 V / 3 A adapter, checked afterwards): program uptime 6 964 s (1 h 56 min) with no restart, 13 551 detection runs, average inference 89.03 ms, RSS 8.66 MB, free RAM 27.4 MB, SoC 66.6 C, 198 817 frames encoded, flash errors 0.
- Not measured: accuracy, multi-viewer load, stability over days, the NPU's own utilisation, power draw, the chip's temperature limit.
