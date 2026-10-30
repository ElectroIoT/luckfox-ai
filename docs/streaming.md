# Streaming (Phase 4) – status

Code: `src/streaming/streamer.{h,cc}` (H.264 hardware encoder bound to the camera, RTSP via the Rockchip `librtsp`, JPEG snapshots), `src/webserver/http_server.{h,cc}` (small HTTP server), `src/system/config.{h,cc}` + `sysinfo.{h,cc}`, program `src/core/aid_core.cc`. Config example: `config/aid.conf.example` (the real file with the token lives only on the board).

## What is tested where
**On the PC (host compiler, `bash scripts/test.sh`): 50 checks, 0 failed.** Config parsing and range checks, HTTP request parsing, and a real server on loopback: wrong token → 401, bearer token and `?token=` → 200, unknown path → 404, POST → 405, garbage → 400, oversized header → 431, token shorter than 16 characters refused, no token → loopback only. This testing found a real bug (handlers returned a body but the status stayed 404); it is fixed.

**On the board: not yet working end to end.**
- First start of `aid-core` (2026-10-30): the camera, ISP and both encoders were created, then `RK_MPI_SYS_Bind(VI->JPEG)` failed with `0xa0028008`: RKMPI refused a second hardware bind of the same VI channel. The program exited cleanly with that message. Code fix written (JPEG encoder is no longer bound; a frame is pulled from VI and sent to it, the same method the Phase 3 snapshot tool used). **The fixed version has not been run on the board.**
- At the same moment the board's flash controller failed again (second flash incident, below).

## Design points
- Video never goes through the CPU: VI → H.264 encoder by hardware bind; the RTSP thread only copies encoded packets into the RTSP server.
- HTTP: `/health`, `/status.json`, `/snapshot.jpg`. With `http_token` set (>= 16 characters) it listens on the network and requires the token; without a token it listens on 127.0.0.1 only. RTSP (`rtsp_demo`) has **no authentication**, as with the stock app. That is a known gap, to be handled in the security phase.
- Measured values the status endpoint is meant to report (frames, fps, kbit/s, capture-to-encoded latency, memory, temperature) are computed by the program itself; none has been measured on the board yet.

## Second flash incident `[MEASURED]` 2026-10-30
Right after copying `aid-core` (1.8 MB) to the board's flash, stopping `rkipc` and starting `aid-core`, the same flash errors appeared as in the first incident (`rockchip-sfc ... DMA wait for transfer finish timeout`, `xfer data failed ret -110`, `ubi0 error -5`, root filesystem read-only; commands such as `wc` and `netstat` failed with `Input/output error`). A restart fixed it (0 flash errors afterwards). The kernel log is in `docs/diag_dmesg_flash_error2_2026-10-30.txt`; its first lines had already scrolled away, so what happened just before is unknown.
Both incidents happened while the ISP/media pipeline was being started or stopped and programs were being read from flash. Hypotheses, none proven: (1) the power supply sags when the camera, ISP and encoders start, which would also disturb the flash; (2) bus/DMA contention between the media pipeline and the flash controller; (3) a marginal flash chip or board. Earlier single runs of `aid-camera` and the stock app showed no errors.
Safeguards used from now on: one test at a time, a flash-error check after each, and a restart after every error.
