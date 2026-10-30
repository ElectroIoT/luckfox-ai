# How to use luckfox-ai

For installation see `docs/installation.md`. This page is about daily use once the board is running.

![Dashboard illustration](images/dashboard.svg)
*Illustration of the dashboard, not a screenshot.*

## The addresses
| What | Address |
|---|---|
| Dashboard | `http://<board-ip>:8080/` (user `admin`, the password you set) |
| Video (VLC, Home Assistant, ffmpeg) | `rtsp://<board-ip>:554/live/0` |
| Picture for scripts | `http://<board-ip>:8080/snapshot.jpg?token=<http_token>` |
| Status for scripts | `http://<board-ip>:8080/status.json?token=<http_token>` |

Find the board's IP in your router's client list or on the Dashboard page itself. Give it a fixed address in the router (DHCP reservation) so the links do not change.

## Watching
- **Dashboard:** live picture with boxes, camera and AI numbers, what is in view now, CPU / memory / temperature, **system flash free**, and **memory card free** (or "RAM only").
- **Live camera:** bigger picture, "Save snapshot" (downloads to your PC), **"Take photo (keep on board)"** (stored on the board, on the card if there is one), the list of stored photos, resolution.
- **VLC:** Media, Open Network Stream, paste the RTSP address. Latency is about half a second to one second.

## Teaching it what matters (AI detection page)
- **Classes:** tick what you care about (person, car, dog...). Unticked classes are ignored.
- **Confidence:** raise it if you get false alarms (an INT8 model on a small NPU makes mistakes), lower it if it misses people.
- **Zones:** draw rectangles on the picture; only objects whose centre is inside a zone count. Use this to ignore the street or a TV.
- **Cooldown:** minimum seconds between two events of the same type (default 30).
Changes apply immediately (no restart), except resolution and ports.

## Events
Event history lists person / vehicle / animal / motion / object entered / disappeared / camera lost / system online, each with a picture. Filter by type, export CSV or JSON, delete old ones. Without a memory card the history is in RAM and is lost when the board restarts.

## Alerts on your phone
- **Telegram** (picture, optional names, photo and video on request): `docs/telegram.md`.
- **Home Assistant** (sensors, automations, notifications): `docs/home-assistant.md`.

## Day-to-day tasks
| I want to | Do this |
|---|---|
| Change the password | Settings, "Change password". Or on the board: `/root/aid/aid-core --set-password` |
| See free flash / card space | Dashboard page (System flash, Memory card) |
| Restart the camera program | System page, "Restart service" (about 12 s) |
| Reboot / shut down the board | System page; shut down before pulling the power if you can |
| See what went wrong | System page, log; or `ssh root@<ip> tail -50 /tmp/aid-core.log` |
| Back up my settings | `bash scripts/backup.sh` on the PC |
| Go back to the stock camera program | `bash scripts/uninstall.sh` |
| Update the program | `bash scripts/build.sh` then `BOARD=root@<ip> bash scripts/deploy.sh` |

## Good practice
- Use a 5 V / 3 A power adapter and a short thick cable (see `docs/firmware.md`, "Power").
- The board is on your LAN without encryption: do not forward ports to it. Read `docs/security.md`.
- Change the board's root password (`passwd` over SSH) and consider disabling telnet.
- Aim the camera where people face it and add light, for both detection and (optional) names.
