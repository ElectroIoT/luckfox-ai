# luckfox-ai – AI vision platform for the Luckfox Pico Pro (RV1106G2)

Work in progress. Read `docs/hardware.md` (what was measured on the real board), `docs/architecture.md` (plan) and `docs/camera.md` (camera service, Phase 3).

Status (2026-10-30): hardware identified and probed; stock camera, Luckfox YOLOv5 example and our own camera tool (`aid-camera`: status, fps, JPEG snapshot) run on the board. Nothing else of the brief (streaming service, AI service of our own, events, dashboard, MQTT, GPIO, security) is implemented yet.

Build (WSL Ubuntu 22.04 with the Luckfox SDK and `luckfox_pico_rkmpi_example` checked out in `$HOME`): `bash scripts/build.sh`
Deploy to the board over SSH key login: `BOARD=root@<ip> bash scripts/deploy.sh`
