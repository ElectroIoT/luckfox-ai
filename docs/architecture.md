# Architecture proposal (draft for approval)

Status 2026-10-30. Everything here is a plan unless marked `[MEASURED]`. Nothing in this file is implemented yet.

## Decision: write our own small daemon, do not extend `rkipc`

| | Extend stock `rkipc` | Own daemon built from the Luckfox example |
|---|---|---|
| Size of code to understand | ≈ 208 000 lines of C/C++ in `project/app/rkipc` (includes bundled code) `[MEASURED]` | ≈ 1 100 lines in `example/luckfox_pico_rtsp_yolov5` `[MEASURED]` |
| Already works on this board | yes: RTSP, ISP tuning, JPEG, motion `[TESTED ON HARDWARE]` | yes: camera + NPU + H.264 + RTSP ran `[TESTED ON HARDWARE]` |
| Adding detection thread, zones, events, MQTT | invasive changes in a big code base (unverified: I have not read its internals) | designed in from the start |
| RAM | `rkipc` ≈ 29 MB available with it running `[MEASURED]` | similar; demo process RSS 11.2 MB `[MEASURED]` |
| Risk | hard to maintain, upstream changes | we must rebuild small features ourselves (snapshots, exposure settings, config) |

Recommendation: **own daemon** (`luckfox-aid`). Keep `rkipc` as the fallback: the board boots it unless we deliberately replace it, and a reboot restores it.

## Proposed process layout (2 processes, to fit ~29 MB of free RAM)
1. `aid-core` (C++): camera (RKMPI VI) → frame path → NPU detection → event engine → H.264 + JPEG encode → RTSP/HTTP snapshot → MQTT client → GPIO. One process, a few threads, no heavy frameworks.
2. `aid-web` (C or C++, tiny HTTP server): serves the dashboard files, login, REST/JSON API, talks to `aid-core` over a local socket. Kept separate so a web crash never stops the camera.

Storage: config in a small JSON file on flash; snapshots and event database on microSD (not yet tested; flash is wear-limited and small). Database choice (SQLite vs JSON lines) is decided after measuring RAM and the SD card.

## Pipeline changes versus the example (all to be verified by measurement)
- The example converts NV12→BGR, resizes and letterboxes on the CPU with OpenCV. The board has an RGA 2D engine (`librga` is present on it). Plan: try RGA for convert/resize, compare CPU load before/after.
- Run inference at a configurable interval (the example runs it on every frame) and put timing around it, so inference latency and FPS are measured, not guessed.
- Stream the clean video and send overlays/boxes as data to the dashboard, or draw them only for the live view; decide after seeing the CPU cost.

## Start-up and recovery
- Replace `rkipc` at boot only after backing up `/oem/usr/bin/RkLunch.sh` and the `S21appinit` hook.
- The example kills `rkipc` and the USB `udhcpc` (which holds the old RTSP socket) before starting; our init script must do the same. `[TESTED ON HARDWARE]`
- No hardware watchdog device was found `[MEASURED]`; recovery will be a software supervisor plus reboot-on-repeated-failure, to be designed and tested.

## Known open questions
1. Safe operating temperature of the RV1106 (chip reached 71.9 °C with the demo running `[MEASURED]`); needs the datasheet value.
2. Real FPS and inference latency (not measured yet).
3. Which model: the example ships a `yolov5.rknn`; we still must convert our own with RKNN-Toolkit2 1.6.0 (runtime on board is 1.6.0) and compare accuracy/speed of yolov5n / yolov5s_relu / yolo11n.
4. microSD behaviour on this board.
5. Security hardening (telnet, adb, default root password) is scheduled for the security phase but should be done before exposing the board outside the home network.

## Next implementation steps
1. Create the project skeleton in `D:\My Project\luckfox-ai` (folders from the brief) with a CMake build against the SDK toolchain.
2. Phase 3: camera service (frame grab, JPEG snapshot, status) — first version of `aid-core` without AI.
3. Phase 4: RTSP stream and HTTP snapshot from `aid-core`; timing/FPS instrumentation.
4. Phase 5: install RKNN-Toolkit2 1.6.0 in WSL, convert/benchmark models on the board.
