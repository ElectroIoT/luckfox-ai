# Roadmap and advice

What is worth doing next, roughly in order of value per effort. Nothing here is promised or measured; the notes say what each item needs and what could go wrong.

## Do these first (cheap, big effect)
| Item | Why | Notes |
|---|---|---|
| Test the microSD path with a real card | The code and unit tests exist, the hardware path does not | `docs/sd-card.md`, "What to check". If the board does not auto-mount the card, the logs and `dmesg` show what is needed |
| Run the Telegram bridge for a week | Real Telegram + real family faces will find the small problems | `docs/telegram.md`. Try `face_match` values; collect pictures where names were wrong |
| Change the board's root password and turn off telnet/adb | The stock firmware leaves both open | `docs/security.md` |
| Fixed IP / DHCP reservation, and a name | Links and Home Assistant stop breaking | Router setting; a hostname (`luckfox-ai.local`) via avahi/mDNS needs a package the SDK would have to build |
| A second power supply test | The flash-controller timeouts may be power-related | Log `dmesg` for `rockchip-sfc` errors over several days; see `docs/firmware.md` |

## Features worth adding
| Feature | Effort | What it needs / risk |
|---|---|---|
| **Video recording to the card** (motion-triggered clips, ring buffer) | Medium | The H.264 stream already exists; needs an MP4/TS muxer (small, can be written) and a pre-roll buffer in RAM (a few MB). Card wear: use a high-endurance card |
| **Names of known people on the board** | Large | Face detector (RetinaFace) + embedding (MobileFaceNet/ArcFace) converted with RKNN-Toolkit2 1.6.0 for the RV1106 INT8 NPU, plus a second inference after each person detection (RAM is the limit: about 28 MB free). Accuracy must be measured; far faces stay hard. Until then use the bridge |
| **Better model** (YOLO11n or YOLOv8n-style) | Medium | Listed as supported for RV1106 in Rockchip's model zoo (limited), not tried; needs conversion with a runtime-compatible toolkit and new post-processing. Compare precision on your own scene before switching |
| **RGA preprocessing** | Small to medium | The picture preparation costs 34-39 ms of CPU per detection; the RGA 2D engine would probably do it in a few ms and free CPU for a shorter interval |
| **Per-class rules and tracking IDs** | Medium | "Alert only for persons staying more than 10 s", "ignore parked car"; a small IoU tracker on the CPU |
| **Privacy masks** | Small | Black rectangles on snapshots and detection ignore regions (zones already ignore areas for detection) |
| **Time sync before first event** | Small | The board's clock is wrong until NTP answers; hold events or set the time from the browser/bridge at start |
| **Static IP and Wi-Fi settings page** | Medium | Ethernet works; Wi-Fi needs a USB dongle driver in the firmware (a firmware rebuild, not done) |
| **TLS** | Medium | Easiest as a reverse proxy (Caddy/nginx on a Raspberry Pi) in front of port 8080; building TLS into the 128 MB board is not worth it |
| **RTSP password** | Medium | The Rockchip RTSP library has none; a small authenticating proxy on another machine is the practical route |
| **OTA update from the dashboard** | Medium | Upload a signed bundle, replace `/root/aid` files, keep the old one for rollback. Must not write the SPI flash more than once |
| **Hardware watchdog** | Medium | The kernel has no watchdog device; needs a device-tree/kernel change (firmware build). The supervisor is the software substitute |
| **HA camera entity over MQTT / WebRTC** | Small | The Generic Camera integration already works |
| **Multiple users and roles** | Medium | Read-only viewer accounts |
| **Prometheus / `/metrics`** | Small | Nice for graphs of temperature and inference time |

## Advice
- **Placement beats model size.** A camera at 2.5 m height angled down, with light on the scene, gives far better detection and any face matching than a larger model on a bad view.
- **Start with zones and a higher confidence** (55 to 65 %) rather than lowering the cooldown; a small INT8 model produces occasional false objects.
- **Keep the board's data on RAM or the card, never the SPI flash.** The flash controller faulted four times during development; the design avoids writing it at run time.
- **Watch temperature.** The SoC settled at 66 to 67 C in open air; in a closed box add airflow or a small heatsink.
- **Do not treat this as a security product.** There is no encryption, the RTSP stream has no password, and the detector is small. It is a hobby/home-automation camera.
- **Back up the settings** (`scripts/backup.sh`) before every update, and update from the PC with `scripts/deploy.sh`.
- **Contribute test results.** The most valuable contribution is a report of what worked on your board (see `CONTRIBUTING.md`): the project labels everything as PC-tested, board-tested or untested for a reason.
