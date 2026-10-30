# Camera service (Phase 3)

Code: `src/camera/` (`camera.h`, `camera.cc`, `aid_camera_cli.cc`). Build: `bash scripts/build.sh` in WSL. Deploy: `bash scripts/deploy.sh`.

## What exists
`aid-camera` command-line tool on top of the `aid::Camera` class:

| Command | What it does | Status |
|---|---|---|
| `aid-camera status` | Read-only JSON: sensor name from sysfs, number of V4L nodes, whether stock `rkipc` runs. Works while `rkipc` runs. | tested on the board |
| `aid-camera fps [sec] [--width W --height H --fps N]` | Opens the camera (ISP 3A + VI) and counts frames | tested on the board at 1920x1080 only |
| `aid-camera snapshot <file> [--width --height --fps --quality]` | Hardware JPEG of one frame (10 warm-up frames are discarded first, a guess) | tested on the board at 1920x1080 only |

Not implemented yet from the brief: exposure, brightness, contrast controls; frame-rate setting is wired (`SAMPLE_COMM_ISP_SetFrameRate`) but untested; video recording; camera restart; resolution list from the sensor.

## Rules the code follows
- The sensor is discovered from `/sys/class/video4linux/*/name` (names like `m00_b_sc3336 4-0030`); no sensor or `/dev/video*` path is hard-coded.
- The camera has one owner. `Camera::open()` refuses while `rkipc` runs; stop it with `scripts/stop_stock_camera.sh` (on the board). A board reboot starts `rkipc` again.

## Measured on the board (2026-10-30, one run each)
- `aid-camera fps 6` at 1920x1080: 180 frames in 5.963 s = **30.02 fps** at the VI channel (this is the rate frames arrive from the camera/ISP, not an end-to-end stream rate).
- `aid-camera snapshot` 1920x1080, quality 85: 92 383 bytes, hardware encode **4.05 ms** (SendFrame→GetStream); the picture was copied to `docs/samples/snap_1080.jpg` and looks like a valid colour image (object near the lens, blurry centre; dim scene; no colour calibration done).
- Not measured: 2304x1296 and 1280x720 (they were attempted in a failed run, see below), longer runs, CPU use of the tool, repeated open/close.

## Problems found while testing
1. **"no frame" at 1 s timeout.** The first frame after `open()` took longer than 1 s (error `0xa008800e` from `RK_MPI_VI_GetChnFrame`). The first-frame timeout is now 5 s; with that the test passed. The actual time to first frame is not measured yet.
2. **Flash read errors on the board during a burst of tests (important).** After 8 quick open/close cycles of the camera (ISP init/deinit each time), the board's root filesystem went read-only and basic commands (`head`, `sort`) failed with `Input/output error`. Kernel log (`docs/diag_dmesg_flash_error_2026-10-30.txt`): `rockchip-sfc ffac0000.spi: DMA wait for transfer finish timeout`, `xfer data failed ret -110`, then `ubi0 error ... error -5 while reading ... PEB 392/393`, then UBIFS went read-only. A restart with `/bin/busybox reboot` restored everything (0 flash errors afterwards, root fs writable again), so the flash cells themselves were not damaged. **Cause unknown.** Possible factors (none proven): memory/DMA pressure with the media pool exhausted, the heat (chip at ~70 °C), or power-supply dips. After the fix I ran a single `fps` and a single `snapshot` test with a flash-error check after each: 0 errors. Until the cause is known: avoid rapid repeated camera open/close, watch `dmesg | grep -i sfc`, keep the board on a good power supply.
3. `RkLunch-stop.sh` is not on the PATH, so program-internal calls to it fail; use `scripts/stop_stock_camera.sh`.
