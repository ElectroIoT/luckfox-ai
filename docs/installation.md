# Installation and development guide (Windows 11)

## What you need
- Windows 11 PC with WSL2 and **Ubuntu 22.04** (`wsl --install -d Ubuntu-22.04`, administrator PowerShell, restart).
- A Luckfox Pico Pro with the official Buildroot firmware (see `firmware.md`), connected by **Ethernet**, powered from a 5 V / 2 A adapter, with the SC3336 camera attached (power off first; metal contacts of the ribbon toward the chip side).
- The board's IP address (look in your router's device list; the examples use 192.168.1.160). Reserve it in the router so it does not change.

## 1. One-time PC setup (inside Ubuntu)
```bash
sudo apt-get update
sudo apt-get install -y git make gcc g++ cmake unzip file openssl openssh-client
git clone --depth 1 https://github.com/LuckfoxTECH/luckfox-pico.git ~/luckfox-pico          # SDK, about 4.6 GB on disk
git clone --depth 1 https://github.com/LuckfoxTECH/luckfox_pico_rkmpi_example.git ~/luckfox_pico_rkmpi_example
git clone <this repository> luckfox-ai
```
`scripts/build.sh` uses `~/luckfox-pico` (compiler) and `~/luckfox_pico_rkmpi_example` (Rockchip headers, libraries, the YOLOv5 model). Other locations: set `LUCKFOX_SDK_PATH` and `LUCKFOX_RKMPI_PATH`.
Working in `D:\My Project\luckfox-ai` from Ubuntu works (`/mnt/d/...`); the build itself happens in `~/build-luckfox-ai` on the fast Linux disk.

## 2. SSH key login to the board (once)
```bash
ssh-keygen -t ed25519 -N "" -f ~/.ssh/id_ed25519
ssh-copy-id root@<board-ip>        # asks for the board's root password once (Luckfox default: luckfox)
ssh root@<board-ip> "chown root:root /root"   # the stock image has /root owned by uid 1005, which makes sshd refuse keys
```
Then change the root password (`passwd` on the board) and see `security.md`.

## 3. Build and test
```bash
cd luckfox-ai
bash scripts/test.sh      # 546 PC-side checks, about 15 s; no board needed
bash scripts/build.sh     # cross-compiles aid-core, aid-supervisor, aid-camera into ./build/
```

## 4. Install on the board
```bash
BOARD=root@<board-ip> bash scripts/install.sh
```
It backs up the settings, copies everything (program 0.5 MB stripped, dashboard, 7.6 MB model), creates `/root/aid/aid.conf` with a random access token, sets a random dashboard password (saved in `~/.aid_admin_password` on the PC; user **admin**), installs `/etc/init.d/S99aid` and starts the service. About 1 minute later: `http://<board-ip>:8080/`.
`--no-autostart` skips the boot script. Run it again to update: it keeps `aid.conf` and the password. The installer refuses to continue if the board's log already shows flash errors; reboot it first.

## 5. Daily use
- Dashboard: `http://<board-ip>:8080/`, user admin. Video: `rtsp://<board-ip>:554/live/0` (VLC works).
- Service control on the board: `/etc/init.d/S99aid status|stop|start|restart`. Log: `/tmp/aid-core.log` (RAM; it is gone after a reboot).
- Check on the board: `curl http://127.0.0.1:8080/health`.
- Update after code changes: `bash scripts/build.sh && bash scripts/install.sh` (it stops the service first through the boot script).
- Uninstall: `bash scripts/uninstall.sh` (add `--purge` to delete `/root/aid`); the board reboots into the stock app.

## 6. Recovering from a failed update
See the table in `firmware.md`. In short: the supervisor falls back to the stock app after repeated crashes; `touch /root/aid/disabled` stops autostart; `scripts/restore.sh` brings the settings back; flashing the official image is the last resort.

## 7. Test tools
- `scripts/test.sh`: PC unit tests (config, HTTP server, API, security, events, MQTT with a fake broker, GPIO with a fake sysfs, AI helpers).
- `scripts/web_demo.sh`: runs the real dashboard on `http://127.0.0.1:18090` with fake camera data (login admin / demo-pass-123, a test value that exists only there).
- `build/aid-camera`: command-line camera tool (`status`, `fps`, `snapshot`, `controls`); stop the camera service first because only one program can own the camera.
- `scripts/probe_hardware.sh`: read-only hardware report to run on the board.

## Known quirks
- After a reboot the board's clock is wrong until NTP syncs (about 30 to 60 s), so the first events after a boot can carry a wrong time.
- Stopping `rkipc` (the stock app) also stops the USB-network DHCP client, because it holds the RTSP socket. The USB link keeps its fixed address (172.32.0.93).
- WSL: `wsl.exe -u root` gives a root shell without a password prompt; useful for `apt-get`.
