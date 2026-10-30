# Configuration reference

The board reads `/root/aid/aid.conf` (key=value lines, `#` comments). Most keys can also be changed on the dashboard's Settings and AI detection pages, which write the file for you (mode 0600). A wrong value is refused with a message, never guessed. `config/aid.conf.example` is the commented template. The file holds secrets: never commit it.

"Restart" in the last column means the change only applies after "Restart service". The same is true for `http_token`, the session times, `events_keep`, `snapshots_keep`, all MQTT keys and the GPIO keys; the dashboard tells you when a saved change needs it. Allowed ranges: `events_keep` 10..2000, `snapshots_keep` 0..500, `fps` 1..60, `bitrate_kbps` 64..20000, `gop` 1..600.

## Video
| Key | Default | Meaning | Restart |
|---|---|---|---|
| `width`, `height` | 1920, 1080 | Picture size, max 2304 x 1296 (SC3336). 1280x720 and 960x540 are also tested | yes |
| `fps` | 30 | Frame rate | yes |
| `bitrate_kbps` | 2048 | H.264 constant bit rate | yes |
| `gop` | 60 | Frames between key frames | yes |
| `jpeg_quality` | 80 | Quality of snapshots (1..99) | yes |
| `iq_dir` | `/etc/iqfiles` | ISP tuning files of the SDK | yes |
| `brightness`, `contrast`, `saturation`, `sharpness` | -1 | 0..255, -1 keeps the camera default | no |

## Network and access
| Key | Default | Meaning |
|---|---|---|
| `rtsp_port`, `rtsp_path` | 554, `/live/0` | RTSP address (no password, see security) |
| `http_port` | 8080 | Dashboard and API |
| `http_token` | empty | 16+ characters. Lets scripts read `/snapshot.jpg`, `/status.json`, events and stored photos, and take a stored photo. Empty = no token access. Send `Authorization: Bearer <token>` or `?token=<token>` |
| `session_idle_min`, `session_max_hours` | 15, 8 | Dashboard login lifetime |

## Storage
| Key | Default | Meaning |
|---|---|---|
| `data_dir` | `auto` | `auto` = microSD card if present, else RAM (`/tmp/aid-data`). Or an absolute path. See `docs/sd-card.md` |
| `events_keep` | 300 | Events kept (at least 2000 on a card) |
| `snapshots_keep` | 30 | Event pictures and stored photos kept (at least 1000 events pictures on a card). 0 = no event pictures |
| `web_dir` | `/root/aid/web` | Dashboard files |
| `auth_file` | `/root/aid/auth.json` | Password hash file |

## AI detection
| Key | Default | Meaning |
|---|---|---|
| `detect_enabled` | 1 | Switch the detector on/off |
| `detect_interval_ms` | 500 | Time between detections (100..10000). Each takes about 125 ms of work |
| `detect_conf_pct` | 45 | Minimum confidence in percent (5..99) |
| `detect_classes` | person, car, motorcycle, bicycle, bus, truck, dog, cat | Any COCO class name is accepted |
| `zone` | none | `x1,y1,x2,y2` as fractions of the picture, one line per zone, up to 8 |
| `model_path`, `labels_path` | `/root/aid/model/...` | The RKNN model and its label list |

## Events
| Key | Default | Meaning |
|---|---|---|
| `event_enter_frames` | 2 | Detections in a row that confirm an object |
| `event_cooldown_s` | 30 | Minimum seconds between events of one type |
| `event_absent_s` | 10 | Seconds an object must be gone before "disappeared" |
| `motion_enabled`, `motion_threshold_pct` | 1, 3 | Simple frame-difference motion detection |

## MQTT / Home Assistant
| Key | Default | Meaning |
|---|---|---|
| `mqtt_enabled` | 0 | Turn MQTT on |
| `mqtt_host`, `mqtt_port` | empty, 1883 | Broker |
| `mqtt_user`, `mqtt_password` | empty | Login (the password is never shown again) |
| `mqtt_prefix` | `luckfox-ai` | Topic prefix, no trailing slash |
| `mqtt_device_name` | Luckfox AI Camera | Device name in Home Assistant |
| `mqtt_discovery_prefix` | `homeassistant` | Home Assistant's discovery prefix |

## GPIO (off by default)
`gpio_enabled=1` and `gpio=name,role,pin,active_low` lines. Roles: relay, buzzer, led, alarm (outputs), pir, door (inputs). Pin = Linux GPIO number, `bank*32 + group*8 + x`. **Pins were not verified on the hardware**; check the pinout and `/sys/kernel/debug/gpio` first, because the camera, Ethernet and USB use some pins.

## Telegram bridge (separate file on your PC)
See `tools/telegram_bridge.conf.example` and `docs/telegram.md`.
