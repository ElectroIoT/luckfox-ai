# Telegram alerts, bot menu and names of known people

![Telegram alert flow](images/telegram-flow.svg)

**Status:** the bridge (`tools/telegram_bridge.py`) is tested on the PC with fake Telegram and board servers (34 tests: alerts, the first-run and reboot cursor rules, commands, authorisation, names, errors). The video path (ffmpeg against the board's real RTSP stream) and the loading of the two face models in OpenCV were run for real. It was **not** run against the real Telegram service, and the face matching was **not** tested with real faces or measured for accuracy. Expect small fixes on first use.

## Why a separate program on your PC
The board has about 28 MB of free RAM, no TLS library (Telegram needs HTTPS), no ffmpeg and a clock that is wrong until it learns the time from the network. The bridge therefore runs on any always-on machine in your LAN (Windows PC, Raspberry Pi, a server, the Home Assistant host) and talks to the board over its normal HTTP API with the access token. Nothing extra runs on the board, so the camera's CPU, RAM and flash behaviour does not change.

The bridge needs Python 3.8+ (standard library only). For `/video` it needs `ffmpeg`; for names it needs `opencv-python` and `numpy`.

## 1. Create the bot (2 minutes)
1. In Telegram open **@BotFather**, send `/newbot`, choose a name and a user name ending in `bot`.
2. BotFather answers with a token like `123456789:AAH...`. That token is a password: anyone who has it controls the bot.
3. Open your new bot and press **Start**. Send `/whoami`. It answers "Your chat id is 123456789" (it says "not authorised yet" until you finish step 2: that is normal). Write the number down.

## 2. Configure and run
```bash
cd tools
cp telegram_bridge.conf.example telegram_bridge.conf
# edit it: board_url, board_token (the http_token from the board), telegram_token, allowed_chat_ids
python3 telegram_bridge.py --config telegram_bridge.conf
```
On Windows use `py telegram_bridge.py --config telegram_bridge.conf` in PowerShell. The board token is in `/root/aid/aid.conf` on the board (and in `~/.aid_token` in WSL if you installed with `scripts/install.sh`).

At the first start the bridge notes the newest event number and only alerts for events after it (no flood of old history). Press the **Menu** button next to the text box in Telegram (the bridge registers its commands), or send `/menu`.

### Keep it running
- **Linux / Raspberry Pi:** `tools/aid-telegram.service` is a systemd unit (instructions inside).
- **Windows:** Task Scheduler, "Create Task", trigger "At log on", action `pyw.exe` (or `py.exe`) with arguments `C:\path\to\luckfox-ai\tools\telegram_bridge.py --config C:\path\to\telegram_bridge.conf`, tick "Run whether user is logged on or not" only if you store the password in the task. Or use `docker`/WSL, whatever you already trust.

## 3. What you get
| You do / happens | Result |
|---|---|
| The board confirms a person (or another type from `alert_types`) | A message with the event picture and the time, to every id in `allowed_chat_ids` |
| `/photo` or the **Photo** button | A live picture. With `save_photos_on_board=1` it is also stored on the board (memory card if there is one; list on the dashboard's Live camera page) |
| `/video` or `/video 20` | Records that many seconds (3 to 60) from the RTSP stream, shrinks it to at most 720p H.264 and sends it as a video. Needs ffmpeg; Telegram limits bot uploads to 50 MB |
| `/status` | Camera and AI state, what is in view, temperature, free RAM, **system flash free**, memory card space, uptime, alert state |
| `/events` | The last 8 events |
| `/alerts on` / `/alerts off` | Mute or unmute alerts (until the bridge restarts; set `alerts_enabled=0` for a permanent default) |
| `/whoami` | Shows your chat id (answered to anybody, it reveals nothing else) |

Only the chat ids you list are served. Messages from anybody else are ignored (and logged), except `/whoami` and `/start`.

### What limits the alerts
The board's event engine already applies confirmation frames and a cooldown (`event_cooldown_s`, default 30 s per event type), zones and class filters (Dashboard, AI detection). Set those there; the bridge adds no timing rules of its own.

## 4. Naming known people ("Ravi detected")
Off by default. With it on, a person alert becomes "Ravi detected" (or "Ravi and Priya detected", or "an unknown person detected") with the picture in which the face was found.

How it works: the bridge takes the event picture, finds faces with OpenCV's **YuNet** detector (MIT licence), turns each face into a 128-number description with **SFace** (Apache-2.0) and compares it with the faces you enrolled. If no face can be seen in the event picture it asks the board for up to `recognise_burst` live pictures, one second apart, because the picture at the moment of detection is often a back or side view. All of this runs on **your PC**; the board does nothing extra.

### Set it up
```bash
pip install opencv-python numpy
python3 telegram_bridge.py --config telegram_bridge.conf --download-face-models   # two files, 39 MB, into face_models/
mkdir -p known_faces/Ravi known_faces/Priya
# put 3 to 5 clear, front-facing photos of each person into their folder (jpg or png)
```
Then add to the config: `known_faces_dir=known_faces` and restart the bridge. Its log lists who was enrolled and warns about photos without a face.

### Be realistic
- **Accuracy was not measured here.** A face needs to be big enough: a person across a room seen by a wide-angle camera is often only 30 to 60 pixels wide, which is too small for reliable recognition. Mount the camera where people walk toward it at 1 to 3 metres, at face height, with light on their faces.
- It will sometimes give no name (back of the head, hat, dark picture) and can occasionally give a wrong one. Raise `face_match` (for example 0.45) to get fewer wrong names and more "unknown"; lower it for the opposite. **Do not use a name from this program for anything safety- or security-critical** (door locks, alarms).
- The board itself only knows "person"; it cannot name anybody. Doing this on the board's 0.5 TOPS NPU is possible in principle (it needs a face detector and a face-embedding model converted with RKNN-Toolkit2) but was not attempted, see `docs/ROADMAP.md`.

### Privacy and the law
Face recognition processes biometric data. In many countries (EU GDPR, Illinois BIPA and others) you need the consent of the people you enrol, and recording people in shared or public places can be restricted. Enrol only people who agreed, keep `known_faces/` private (it is git-ignored here), and tell visitors if a camera watches them. The enrolled photos and the models stay on the PC running the bridge. The alert pictures, names and text **do go to Telegram's servers** when sent; bot chats are not end-to-end encrypted.

## Security notes
- Keep `telegram_bridge.conf` private (it holds the board token and the bot token). The bridge never prints tokens; error messages from the network layer do not contain URLs.
- The board token allows: read the live picture and status, read events and their pictures, read stored photos, and **take a photo that is stored on the board**. It cannot change settings, delete events or reach GPIO. If the token leaks, change `http_token` in the dashboard's Settings.
- Board and bridge talk plain HTTP on your LAN. Do not expose the board to the internet; the bridge reaches Telegram on its own outgoing connection, so no port forwarding is needed.
- The RTSP stream used by `/video` has no password (see `docs/security.md`).

## Troubleshooting
| Symptom | Likely cause |
|---|---|
| `config problem: ...` at start | A value is missing or wrong; the message names it |
| "the board refused the token" | `board_token` differs from `http_token` on the board |
| Bot answers `/whoami` but nothing else | Your id is not in `allowed_chat_ids` |
| `/video` says ffmpeg was not found | Install ffmpeg or set `ffmpeg=C:\path\to\ffmpeg.exe` |
| `/video` says ffmpeg failed | Open `rtsp://<board>:554/live/0` in VLC from the same PC; if that fails, the network or the board is the problem |
| Alerts have the wrong time | The board's clock is wrong until it reaches an NTP server; the bridge prints the time the board stored |
| No alerts at all | Check `/status`: "Alerts: on", camera ok. Check that the class is in `detect_classes` on the board and `alert_types` here |
| Alerts stopped after the board rebooted without a memory card | Fixed by design: the bridge notices the event numbering restarted and starts over |
| `face names switched off` | `pip install opencv-python numpy` and run `--download-face-models` |
