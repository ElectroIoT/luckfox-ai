# luckfox-ai: AI camera for the Luckfox Pico Pro (RV1106G2)

Turns a Luckfox Pico Pro with a MIPI camera into a small network camera that runs object detection on its NPU, with a web dashboard, events, Home Assistant (MQTT) and automatic start at boot.
Built and tested on one board (Pico Pro, SC3336 camera, Buildroot firmware 250607). Read the **Status** table: it says what was tested on the board, what only on the PC, and what is missing.

## Quick start
1. Read `docs/installation.md` (Windows 11 + WSL2 + the Luckfox SDK, SSH key login).
2. `bash scripts/test.sh` (PC tests), `bash scripts/build.sh`, `BOARD=root@<ip> bash scripts/install.sh`.
3. Open `http://<board-ip>:8080/` (user `admin`, password in `~/.aid_admin_password` in WSL). Video: `rtsp://<board-ip>:554/live/0`.
4. Change the root password of the board and read `docs/security.md`.

## What it does
- **Camera:** SC3336 through the Rockchip ISP, 30 fps, H.264 hardware encoder, RTSP stream, JPEG snapshots, image controls.
- **AI:** YOLOv5 (COCO classes) on the NPU about every 500 ms; boxes, labels, confidence, zones, counts.
- **Events:** person / vehicle / animal detected, object entered / disappeared, motion, camera lost, system online/offline, with snapshot, cooldown and duplicate suppression; history, filter, export, delete.
- **Dashboard:** Dashboard, Live camera (boxes), AI detection (zones, classes, thresholds), Event history, Settings, System (CPU/RAM/temperature, logs, restart/reboot/shutdown). Login, sessions, CSRF protection.
- **Home Assistant:** MQTT with discovery: person/vehicle/animal/motion sensors, camera and AI status, detection count, temperature, last event, availability.
- **GPIO:** relays, buzzer, LED, alarm outputs; PIR and door inputs (off by default).
- **Operation:** starts at boot, restarts after crashes, falls back to the stock app, reboots itself from a read-only root; install/uninstall/backup/restore scripts.

## Status
| Area | Result | Evidence |
|---|---|---|
| Board identified (Pico Pro, RV1106G2, 128 MB, SC3336, kernel 5.10.160, RKNN runtime 1.6.0, MPP 16e796a4) | **Tested on the board** | `docs/hardware.md` |
| Camera: 30 fps at 2304x1296, 1920x1080, 1280x720; JPEG snapshot; brightness control | **Tested on the board** | `docs/camera.md` |
| RTSP stream (watched in VLC), HTTP snapshot, token protection | **Tested on the board** | `docs/streaming.md` |
| NPU detection: model loads, about 89 ms per inference, detections appear | **Tested on the board** (accuracy **not** measured) | `docs/ai-models.md` |
| Events + snapshots, motion event | Rules **tested on the PC** (84 checks); a motion event with snapshot was seen on the board; person/vehicle events not yet observed there | `tests/test_events.cc` |
| Dashboard pages | **Tested in a browser** against a demo server with fake data (login, 6 pages, phone layout); the API was tested on the board with curl. The real board's pages were **not** opened in a browser (that would need typing the board password into a non-local page). | `scripts/web_demo.sh` |
| Login, sessions, lockout, CSRF, settings validation | **Tested on the PC** (151 checks) and key rules **on the board** | `docs/security.md` |
| MQTT client + Home Assistant discovery | **Tested on the PC only** (fake broker); never against a real broker/HA | `docs/home-assistant.md` |
| GPIO | **Tested on the PC only** (fake sysfs); pins unverified, disabled | `src/gpio/gpio.h` |
| Autostart at boot, crash restart, 26 s cold boot to dashboard | **Tested on the board** | `docs/installation.md` |
| Supervisor reboot on read-only root | Written, **not triggered on purpose** (would need a real flash fault) | `src/core/aid_supervisor.cc` |
| `uninstall.sh`, `restore.sh`, `backup.sh --firmware` | Written, **not run** (`backup.sh` without options and `install.sh` were run) | `scripts/` |
| microSD storage | **Not tested** (no card) | |
| Video recording to a file, H.265 stream, manual exposure/gain setter, per-camera restart | **Not implemented** | |
| Own model conversion (RKNN-Toolkit2) | **Not done**; the Luckfox model is used | `docs/ai-models.md` |
| Stability | **9-minute soak on the final software**: 15 208 frames, 1 034 detections, memory flat (RSS 8.68 MB, 28.04 MB free throughout), 28 to 29 fps, 0 flash errors, temperature 63.6 to 69.6 C and still creeping up. **Hours or days are untested.** | `docs/hardware.md` section 9 |

## Known problems (important)
- **The board's SPI flash controller timed out four times** during development (files unreadable, root filesystem read-only) until a restart. Cause not proven; possibly power, possibly the flash itself. The software avoids flash access while running and the supervisor reboots by itself if it happens. See `docs/streaming.md` and `docs/firmware.md`.
- No encryption (HTTP, MQTT) and no RTSP password; telnet/adb of the stock firmware are still on. See `docs/security.md`.
- Event history is kept in RAM (lost at reboot) unless a microSD card is used.
- Detection quality is unverified; the model is a small INT8 YOLOv5.

## Documents
`docs/hardware.md` (measured board facts) · `docs/firmware.md` · `docs/installation.md` · `docs/camera.md` · `docs/streaming.md` · `docs/ai-models.md` · `docs/home-assistant.md` · `docs/security.md` · `docs/architecture.md`

## License / third-party
The project code has no license file yet (add one before sharing). `src/ai/third_party/rockchip/` is Rockchip code under the Apache License 2.0 (see its `NOTICE.md`). The model and the Rockchip libraries come from the Luckfox SDK and example repositories and are not stored here.
