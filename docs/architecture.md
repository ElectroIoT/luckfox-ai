# Architecture (as built)

One camera appliance process on a 128 MB board, started and watched by a very small supervisor.

```
 boot -> S21appinit (stock rkipc) -> S99aid -> aid-supervisor --stops rkipc--> aid-core
                                                  |  restarts on exit/hang, reboots if / is read-only
 aid-core
   Camera (ISP + VI)  --hardware bind-->  H.264 encoder ---> RTSP server (port 554)
        |  one frame at a time (frame lock)
        +--> JPEG encoder (snapshots)
        +--> DetectionService thread: NV12 -> RGB letterbox (CPU) -> NPU (YOLOv5) -> boxes
                 +--> MotionDetector (80x45 luma grid)
                 +--> EventEngine (confirm, zones, cooldown) --> EventStore (RAM, JSON lines + JPEGs)
                                                               +--> MQTT publisher (Home Assistant)
   HttpServer (port 8080, 1 thread per connection, max 6) --> Api: login/sessions, settings, events,
                 logs, system actions, static dashboard files (held in RAM)
   Gpio (optional), MqttClient (optional), 1 s housekeeping loop (heartbeat file, stats, MQTT states)
```

## Decisions (and why)
| Decision | Reason |
|---|---|
| Own program, not an extension of the stock `rkipc` | `rkipc` is about 208 000 lines; the example we started from is about 1 100. Measured: our whole program adds about 8.6 MB RSS. |
| Single process, threads | 128 MB RAM (about 54 MB visible to Linux, about 28 MB free with everything running). A second process would cost several MB and gain little. The earlier plan (separate web process) was dropped for this reason. |
| Video path in hardware | VI is bound to the H.264 encoder; the CPU only hands encoded packets to the RTSP library. A second hardware bind of one VI channel is refused by RKMPI (error `0xa0028008`), so snapshots and detection pull single frames instead. |
| CPU picture preparation for the NPU | Simple and measurable (34 to 39 ms). The RGA 2D engine would probably be faster; not tried. |
| JSON lines + RAM instead of SQLite | A few hundred events; flash is small and showed controller errors; a bounded text file is easier to reason about. History is lost at reboot unless `data_dir` is a microSD card. |
| Dashboard files held in RAM, code pages locked (`mlockall`) | Reading the flash while the camera runs may be what triggers its controller timeouts (see `streaming.md`); the program avoids flash I/O in steady state. |
| Compiled supervisor | A shell loop starts processes from the flash every few seconds; the C++ supervisor does not. It also reboots the board with the `reboot` system call if `/` turns read-only. |
| No external libraries | MQTT, HTTP, SHA-256/PBKDF2 and JSON helpers are small and unit tested; there is no package manager on the board. |
| GPIO disabled by default | The pinout could not be verified from here; the module never picks a pin. |

## Code layout
| Folder | Content |
|---|---|
| `src/camera` | `Camera` (ISP via rkaiq, VI), `aid-camera` tool, `FrameView` |
| `src/streaming` | `Streamer`: H.264 encoder, RTSP, JPEG snapshots, frame access, statistics |
| `src/ai` | `Detector` (RKNN), `preprocess`, `motion`, `DetectionService`; `third_party/rockchip` (Apache-2.0) |
| `src/events` | `EventEngine` (pure rules), `EventStore` |
| `src/webserver` | `HttpServer`, `Api`, `AuthManager`, `StaticCache` |
| `src/mqtt` | `MqttClient`, Home Assistant discovery |
| `src/gpio` | `Gpio` (sysfs) |
| `src/system` | config, JSON helpers, log ring buffer, system information |
| `src/core` | `aid_core.cc` (wires everything), `aid_supervisor.cc` |
| `web` | dashboard (HTML, CSS, JavaScript; no framework) |
| `services` | `S99aid` boot script |
| `scripts` | build, test, deploy, install, uninstall, backup, restore, demo, probe |
| `tests` | PC-side tests (546 checks) |
| `docs` | this documentation and the measured evidence (`hardware.md`, logs in `diag_*.txt`) |

## Threads in `aid-core`
main loop (100 ms tick), RTSP/encoder reader, detection service, HTTP accept + up to 6 connection threads, MQTT client (if enabled), plus the Rockchip libraries' own threads (ISP, MPP, rockit). 15 threads were counted on the board with one viewer.

## Resource budget (measured, one board, one scene)
- RAM: aid-core RSS 8.6 MB; free RAM about 28 MB; the 66 MB CMA pool for video buffers is fully used by the media pipeline.
- CPU: 25 to 29 % of the whole system with video + detection every 500 ms + one RTSP viewer; aid-core itself 11 to 26 %.
- Temperature: 62 to 66 C at the SoC sensor. The chip's limit was not looked up.
- Flash: program 0.5 MB, model 7.6 MB, dashboard about 100 KB in `/root/aid`; nothing written at run time.
