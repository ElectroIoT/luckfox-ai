#!/usr/bin/env python3
"""luckfox-ai Telegram bridge.

Runs on an always-on machine in your LAN (PC, Raspberry Pi, Home Assistant host, NAS), NOT on the
board: the board has no TLS and very little RAM. It talks to the board over its HTTP API with the
machine token and to Telegram over HTTPS.

  * sends a Telegram message with the event picture when the board detects a person (or other types)
  * optional: names the person ("Ravi detected") by matching the face against photos you enrolled
  * bot menu / commands: photo, video, status, last events, alerts on/off
  * only chat ids listed in allowed_chat_ids are served

Python 3.8+, standard library only (face names need opencv-python and numpy, video needs ffmpeg).
Run:  python telegram_bridge.py --config telegram_bridge.conf
NOT tested against the real Telegram service or a real board yet; the PC tests use fake servers.
"""
import argparse
import json
import logging
import math
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

log = logging.getLogger("aid-telegram")

EVENT_TITLES = {
    "person_detected": "Person detected",
    "vehicle_detected": "Vehicle detected",
    "animal_detected": "Animal detected",
    "motion_detected": "Motion detected",
    "object_entered": "Object entered",
    "object_disappeared": "Object disappeared",
    "camera_lost": "Camera lost",
    "camera_restored": "Camera restored",
    "system_online": "System online",
    "system_offline": "System offline",
    "input_changed": "Input changed",
}
EVENT_ICONS = {"person_detected": "\U0001F464", "vehicle_detected": "\U0001F697", "animal_detected": "\U0001F43E",
               "motion_detected": "\U0001F3C3"}
MAX_UPLOAD = 49 * 1024 * 1024   # Telegram bots may upload up to 50 MB


class ConfigError(Exception):
    pass


class NetError(Exception):
    pass


class BoardError(Exception):
    pass


class TelegramError(Exception):
    pass


# ------------------------------------------------------------------ configuration
class Config(object):
    def __init__(self):
        self.board_url = ""
        self.board_token = ""
        self.telegram_token = ""
        self.telegram_api = "https://api.telegram.org"
        self.allowed_chat_ids = []
        self.alert_types = ["person_detected"]
        self.alerts_enabled = True
        self.poll_interval_s = 2.0
        self.video_seconds = 10
        self.rtsp_url = ""
        self.ffmpeg = "ffmpeg"
        self.save_photos_on_board = True
        self.known_faces_dir = ""
        self.face_models_dir = "face_models"
        self.face_match = 0.363          # cosine score threshold recommended for OpenCV's SFace
        self.recognise_burst = 4         # extra live pictures tried (1 s apart) when no face is found


def _bool(key, v):
    if v.lower() in ("1", "true", "yes", "on"):
        return True
    if v.lower() in ("0", "false", "no", "off"):
        return False
    raise ConfigError("%s must be 0 or 1" % key)


def _num(key, v, lo, hi, kind=float):
    try:
        n = kind(v)
    except ValueError:
        raise ConfigError("%s must be a number" % key)
    if not lo <= n <= hi:
        raise ConfigError("%s must be between %s and %s" % (key, lo, hi))
    return n


def parse_config(text):
    raw = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        raw[k.strip()] = v.strip()
    c = Config()
    known = set(vars(c).keys())
    for k in raw:
        if k not in known:
            raise ConfigError("unknown setting: %s" % k)
    for k in ("board_url", "board_token", "telegram_token", "telegram_api", "rtsp_url", "ffmpeg",
              "known_faces_dir", "face_models_dir"):
        if k in raw:
            setattr(c, k, raw[k])
    c.board_url = c.board_url.rstrip("/")
    c.telegram_api = c.telegram_api.rstrip("/")
    if not c.board_url.startswith(("http://", "https://")):
        raise ConfigError("board_url must start with http:// or https://")
    if not c.board_token:
        raise ConfigError("board_token is required (the http_token of the board)")
    if ":" not in c.telegram_token:
        raise ConfigError("telegram_token is required (from @BotFather)")
    ids = [x.strip() for x in raw.get("allowed_chat_ids", "").split(",") if x.strip()]
    if not ids:
        raise ConfigError("allowed_chat_ids is required (send /whoami to the bot to learn yours)")
    try:
        c.allowed_chat_ids = [int(x) for x in ids]
    except ValueError:
        raise ConfigError("allowed_chat_ids must be numbers separated by commas")
    if "alert_types" in raw:
        c.alert_types = [x.strip() for x in raw["alert_types"].split(",") if x.strip()]
        for t in c.alert_types:
            if not all(ch.islower() or ch == "_" for ch in t):
                raise ConfigError("bad alert type: %s" % t)
    if "alerts_enabled" in raw:
        c.alerts_enabled = _bool("alerts_enabled", raw["alerts_enabled"])
    if "save_photos_on_board" in raw:
        c.save_photos_on_board = _bool("save_photos_on_board", raw["save_photos_on_board"])
    if "poll_interval_s" in raw:
        c.poll_interval_s = _num("poll_interval_s", raw["poll_interval_s"], 1, 60)
    if "video_seconds" in raw:
        c.video_seconds = _num("video_seconds", raw["video_seconds"], 3, 60, int)
    if "face_match" in raw:
        c.face_match = _num("face_match", raw["face_match"], 0.1, 0.9)
    if "recognise_burst" in raw:
        c.recognise_burst = _num("recognise_burst", raw["recognise_burst"], 0, 10, int)
    if not c.rtsp_url:
        host = urllib.parse.urlparse(c.board_url).hostname
        c.rtsp_url = "rtsp://%s:554/live/0" % host
    return c


# ------------------------------------------------------------------ HTTP helpers
def http_request(url, method="GET", headers=None, data=None, timeout=15):
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except (urllib.error.URLError, OSError) as e:
        # the message never contains the URL, so tokens stay out of the logs
        raise NetError(str(getattr(e, "reason", e)))


def encode_multipart(fields, files):
    boundary = "----aid" + uuid.uuid4().hex
    parts = []
    for k, v in fields.items():
        parts.append(('--%s\r\nContent-Disposition: form-data; name="%s"\r\n\r\n%s\r\n' % (boundary, k, v)).encode("utf-8"))
    for k, (filename, data, mime) in files.items():
        parts.append(('--%s\r\nContent-Disposition: form-data; name="%s"; filename="%s"\r\nContent-Type: %s\r\n\r\n'
                      % (boundary, k, filename, mime)).encode("utf-8") + data + b"\r\n")
    parts.append(("--%s--\r\n" % boundary).encode("utf-8"))
    return b"".join(parts), "multipart/form-data; boundary=" + boundary


# ------------------------------------------------------------------ board
class Board(object):
    def __init__(self, url, token, request=http_request):
        self.url, self.token, self.request = url, token, request

    def _call(self, path, query=None, method="GET", timeout=15):
        url = self.url + path + ("?" + urllib.parse.urlencode(query) if query else "")
        headers = {"Authorization": "Bearer " + self.token}
        if method != "GET":
            headers["X-Requested-With"] = "aid-dashboard"
        status, body = self.request(url, method=method, headers=headers, data=b"" if method == "POST" else None, timeout=timeout)
        if status == 401:
            raise BoardError("the board refused the token (check board_token)")
        if status != 200:
            raise BoardError("board answered HTTP %d for %s" % (status, path))
        return body

    def events_after(self, after_id, limit=50):
        """Returns (last_id, events oldest first)."""
        data = json.loads(self._call("/api/events", {"after_id": after_id, "limit": limit}).decode("utf-8"))
        evs = sorted(data.get("events", []), key=lambda e: e["id"])
        return int(data.get("last_id", 0)), evs

    def event_snapshot(self, name):
        return self._call("/api/event_snapshot/" + urllib.parse.quote(name))

    def snapshot(self):
        return self._call("/snapshot.jpg", timeout=20)

    def capture(self):
        """Takes a photo that the board also stores (on its memory card if it has one)."""
        name = json.loads(self._call("/api/capture", method="POST", timeout=20).decode("utf-8"))["name"]
        return self._call("/api/capture/" + urllib.parse.quote(name))

    def status(self):
        return json.loads(self._call("/status.json").decode("utf-8"))


# ------------------------------------------------------------------ Telegram
class Telegram(object):
    def __init__(self, token, api="https://api.telegram.org", request=http_request):
        self.base = "%s/bot%s/" % (api, token)
        self.request = request

    def call(self, method, params=None, timeout=15):
        status, body = self.request(self.base + method, method="POST", headers={"Content-Type": "application/json"},
                                    data=json.dumps(params or {}).encode("utf-8"), timeout=timeout)
        try:
            res = json.loads(body.decode("utf-8"))
        except ValueError:
            raise TelegramError("Telegram answered HTTP %d (not JSON)" % status)
        if not res.get("ok"):
            raise TelegramError("%s: %s" % (method, res.get("description", "HTTP %d" % status)))
        return res.get("result")

    def send_message(self, chat, text, keyboard=None):
        p = {"chat_id": chat, "text": text}
        if keyboard:
            p["reply_markup"] = {"inline_keyboard": keyboard}
        return self.call("sendMessage", p)

    def _upload(self, method, field, chat, filename, data, mime, caption, timeout=60):
        body, ctype = encode_multipart({"chat_id": str(chat), "caption": caption or ""}, {field: (filename, data, mime)})
        status, resp = self.request(self.base + method, method="POST", headers={"Content-Type": ctype}, data=body, timeout=timeout)
        try:
            res = json.loads(resp.decode("utf-8"))
        except ValueError:
            raise TelegramError("Telegram answered HTTP %d (not JSON)" % status)
        if not res.get("ok"):
            raise TelegramError("%s: %s" % (method, res.get("description", "HTTP %d" % status)))
        return res.get("result")

    def send_photo(self, chat, jpeg, caption=""):
        return self._upload("sendPhoto", "photo", chat, "photo.jpg", jpeg, "image/jpeg", caption)

    def send_video(self, chat, mp4, caption=""):
        return self._upload("sendVideo", "video", chat, "clip.mp4", mp4, "video/mp4", caption, timeout=120)

    def get_updates(self, offset, wait=25):
        return self.call("getUpdates", {"offset": offset, "timeout": wait, "allowed_updates": ["message", "callback_query"]},
                         timeout=wait + 10)

    def answer_callback(self, cid):
        try:
            self.call("answerCallbackQuery", {"callback_query_id": cid})
        except (TelegramError, NetError):
            pass

    def set_commands(self, commands):
        self.call("setMyCommands", {"commands": [{"command": c, "description": d} for c, d in commands]})


# ------------------------------------------------------------------ face names (optional)
def cosine(a, b):
    dot = sum(x * y for x, y in zip(a, b))
    na = math.sqrt(sum(x * x for x in a))
    nb = math.sqrt(sum(y * y for y in b))
    return dot / (na * nb) if na and nb else 0.0


def match_name(feature, gallery, threshold):
    """gallery: {name: [feature, ...]}. Returns (name or None, best score)."""
    best_name, best = None, -1.0
    for name, feats in gallery.items():
        for f in feats:
            s = cosine(feature, f)
            if s > best:
                best, best_name = s, name
    if best_name is not None and best >= threshold:
        return best_name, best
    return None, best


FACE_MODELS = {
    "face_detection_yunet_2023mar.onnx":
        "https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx",
    "face_recognition_sface_2021dec.onnx":
        "https://github.com/opencv/opencv_zoo/raw/main/models/face_recognition_sface/face_recognition_sface_2021dec.onnx",
}


def download_face_models(directory):
    os.makedirs(directory, exist_ok=True)
    for name, url in FACE_MODELS.items():
        path = os.path.join(directory, name)
        if os.path.exists(path) and os.path.getsize(path) > 100000:
            continue
        log.info("downloading %s", name)
        status, body = http_request(url, timeout=120)
        if status != 200 or len(body) < 100000:
            raise NetError("download of %s failed (HTTP %d, %d bytes)" % (name, status, len(body)))
        with open(path, "wb") as f:
            f.write(body)


class FaceNamer(object):
    """OpenCV YuNet (face finder, MIT) + SFace (face embedding, Apache-2.0), running on this machine only.

    Enrol people with  <known_faces_dir>/<Name>/*.jpg  (a few clear, front-facing photos each).
    Accuracy was NOT measured. Small, distant or turned-away faces are often not found at all.
    """

    def __init__(self, faces_dir, models_dir, threshold):
        import cv2  # noqa: imported late so the bridge runs without OpenCV
        self.cv2 = cv2
        self.threshold = threshold
        self.det = cv2.FaceDetectorYN.create(os.path.join(models_dir, "face_detection_yunet_2023mar.onnx"), "", (320, 320), 0.8, 0.3, 5000)
        self.rec = cv2.FaceRecognizerSF.create(os.path.join(models_dir, "face_recognition_sface_2021dec.onnx"), "")
        self.gallery = {}
        for person in sorted(os.listdir(faces_dir)):
            pdir = os.path.join(faces_dir, person)
            if not os.path.isdir(pdir):
                continue
            for fn in sorted(os.listdir(pdir)):
                if not fn.lower().endswith((".jpg", ".jpeg", ".png")):
                    continue
                img = cv2.imread(os.path.join(pdir, fn))
                feats = self._features(img) if img is not None else []
                if len(feats) >= 1:
                    # the largest face is the enrolled person
                    self.gallery.setdefault(person, []).append(feats[0])
                else:
                    log.warning("no face found in %s/%s", person, fn)
        log.info("known faces: %s", ", ".join("%s (%d)" % (k, len(v)) for k, v in self.gallery.items()) or "none")

    def _features(self, img):
        h, w = img.shape[:2]
        self.det.setInputSize((w, h))
        _, faces = self.det.detect(img)
        if faces is None:
            return []
        faces = sorted(faces, key=lambda f: -f[2] * f[3])
        return [self.rec.feature(self.rec.alignCrop(img, f)).flatten().tolist() for f in faces]

    def identify(self, jpeg):
        """Returns one entry per face found: (name or None, score)."""
        import numpy as np
        img = self.cv2.imdecode(np.frombuffer(jpeg, np.uint8), self.cv2.IMREAD_COLOR)
        if img is None:
            return []
        return [match_name(f, self.gallery, self.threshold) for f in self._features(img)]


# ------------------------------------------------------------------ the bridge
def format_time(ts_ms):
    return time.strftime("%H:%M:%S", time.localtime(ts_ms / 1000.0))


def join_names(names):
    if len(names) <= 1:
        return "".join(names)
    return ", ".join(names[:-1]) + " and " + names[-1]


def caption_for(ev, names=None, unknown=0):
    icon = EVENT_ICONS.get(ev.get("type"), "⚠️")
    title = EVENT_TITLES.get(ev.get("type"), ev.get("type", "event"))
    if ev.get("type") == "person_detected" and (names or unknown):
        who = list(names or [])
        if unknown:
            who.append("an unknown person" if unknown == 1 else "%d unknown people" % unknown)
        title = join_names(who) + " detected"
    conf = ev.get("conf") or 0
    extra = " (%d%%)" % round(conf * 100) if conf else ""
    return "%s %s%s\n%s" % (icon, title, extra, format_time(ev.get("ts", 0)))


MENU = [
    [("\U0001F4F7 Photo", "photo"), ("\U0001F3A5 Video", "video")],
    [("\U0001F4CA Status", "status"), ("\U0001F5C2 Events", "events")],
    [("\U0001F514 Alerts on", "alerts_on"), ("\U0001F515 Alerts off", "alerts_off")],
]
COMMANDS = [("menu", "Show the menu"), ("photo", "Take a photo"), ("video", "Record a short video"),
            ("status", "Camera status"), ("events", "Last events"), ("alerts", "Alerts on or off"),
            ("whoami", "Show my chat id"), ("help", "Help")]


class Bridge(object):
    def __init__(self, cfg, board, tg, namer=None, run=subprocess.run, sleep=time.sleep):
        self.cfg, self.board, self.tg, self.namer = cfg, board, tg, namer
        self.run, self.sleep = run, sleep
        self.cursor = None
        self.alerts_on = cfg.alerts_enabled
        self.stop = threading.Event()
        self.video_lock = threading.Lock()

    # ---- helpers
    def broadcast_photo(self, jpeg, caption):
        for chat in self.cfg.allowed_chat_ids:
            try:
                self.tg.send_photo(chat, jpeg, caption)
            except (TelegramError, NetError) as e:
                log.warning("cannot send to %s: %s", chat, e)

    def broadcast_text(self, text):
        for chat in self.cfg.allowed_chat_ids:
            try:
                self.tg.send_message(chat, text)
            except (TelegramError, NetError) as e:
                log.warning("cannot send to %s: %s", chat, e)

    # ---- events -> alerts
    def recognise(self, jpeg):
        """Returns (picture to send, known names, number of unknown faces)."""
        if not self.namer or not jpeg:
            return jpeg, [], 0
        picture = jpeg
        faces = self.namer.identify(jpeg)
        tries = 0
        while not faces and tries < self.cfg.recognise_burst:
            tries += 1
            self.sleep(1)
            try:
                picture = self.board.snapshot()
            except (BoardError, NetError):
                break
            faces = self.namer.identify(picture)
        names = []
        for n, _ in faces:
            if n and n not in names:
                names.append(n)
        unknown = sum(1 for n, _ in faces if not n)
        return (picture if faces else jpeg), names, unknown

    def send_alert(self, ev):
        jpeg = None
        if ev.get("snapshot"):
            try:
                jpeg = self.board.event_snapshot(ev["snapshot"])
            except (BoardError, NetError) as e:
                log.warning("event picture not available: %s", e)
        names, unknown = [], 0
        if ev.get("type") == "person_detected":
            try:
                jpeg, names, unknown = self.recognise(jpeg)
            except Exception as e:   # face code must never stop alerts
                log.warning("face recognition failed: %s", e)
        caption = caption_for(ev, names, unknown)
        if jpeg:
            self.broadcast_photo(jpeg, caption)
        else:
            self.broadcast_text(caption)

    def poll_events(self):
        """One poll of the board. Returns the number of alerts sent."""
        if self.cursor is None:
            self.cursor, _ = self.board.events_after(0, 1)   # first run: do not replay old history
            log.info("watching events after #%d", self.cursor)
            return 0
        last_id, events = self.board.events_after(self.cursor, 50)
        if last_id < self.cursor:
            # the board restarted with an empty history: numbering began again
            log.info("event numbering restarted on the board (#%d < #%d)", last_id, self.cursor)
            self.cursor = 0
            last_id, events = self.board.events_after(0, 50)
        sent = 0
        for ev in events:
            if ev["id"] <= self.cursor:
                continue
            self.cursor = ev["id"]
            if self.alerts_on and ev.get("type") in self.cfg.alert_types:
                self.send_alert(ev)
                sent += 1
        return sent

    # ---- commands
    def authorised(self, chat):
        return chat in self.cfg.allowed_chat_ids

    def do_photo(self, chat):
        try:
            jpeg = self.board.capture() if self.cfg.save_photos_on_board else self.board.snapshot()
        except (BoardError, NetError, ValueError, KeyError):
            jpeg = self.board.snapshot()      # storing failed: still send a live picture
        self.tg.send_photo(chat, jpeg, "\U0001F4F7 " + time.strftime("%Y-%m-%d %H:%M:%S"))

    def record_video(self, seconds):
        tmp = tempfile.mkdtemp(prefix="aid-clip-")
        out = os.path.join(tmp, "clip.mp4")
        cmd = [self.cfg.ffmpeg, "-y", "-loglevel", "error", "-rtsp_transport", "tcp", "-i", self.cfg.rtsp_url,
               "-t", str(seconds), "-an", "-c:v", "libx264", "-preset", "veryfast", "-crf", "28",
               "-vf", "scale=-2:'min(720,ih)'", "-pix_fmt", "yuv420p", "-movflags", "+faststart", out]
        try:
            res = self.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=seconds + 40)
            if res.returncode != 0 or not os.path.exists(out):
                raise BoardError("ffmpeg failed: " + (res.stderr or b"").decode("utf-8", "replace")[-200:])
            if os.path.getsize(out) > MAX_UPLOAD:
                raise BoardError("the clip is larger than Telegram's 50 MB limit; use a shorter video_seconds")
            with open(out, "rb") as f:
                return f.read()
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def do_video(self, chat, seconds):
        seconds = max(3, min(int(seconds), 60))
        if not self.video_lock.acquire(False):
            self.tg.send_message(chat, "A video is already being recorded, try again in a moment.")
            return
        try:
            self.tg.send_message(chat, "\U0001F3A5 Recording %d seconds..." % seconds)
            try:
                mp4 = self.record_video(seconds)
            except FileNotFoundError:
                raise BoardError("ffmpeg was not found; install it or set ffmpeg=<path> in the config")
            self.tg.send_video(chat, mp4, "\U0001F3A5 %d s, %s" % (seconds, time.strftime("%Y-%m-%d %H:%M:%S")))
        finally:
            self.video_lock.release()

    def status_text(self):
        s = self.board.status()
        sysd, ai, cam = s.get("system", {}), s.get("ai", {}), s.get("camera", {})
        present = ", ".join("%s %d" % kv for kv in sorted(s.get("present", {}).items())) or "nothing"
        sd = sysd.get("sd", {})
        card = "no card"
        if sd.get("in_use"):
            card = "card %.0f of %.0f MB free" % (sd.get("free_mb", 0), sd.get("total_mb", 0))
        elif sd.get("present"):
            card = "card present, not used"
        up = int(s.get("uptime_s", 0))
        return ("Camera: %s, %.0f fps\nAI: %s, %.0f ms per run\nSeeing now: %s\nTemperature: %.0f C, free RAM %.0f MB\n"
                "System flash free: %.0f of %.0f MB\nStorage: %s\nCamera program running for: %dh %02dm\nAlerts: %s") % (
            "ok" if cam.get("ok") else "NOT OK", s.get("stream", {}).get("recent_fps", 0),
            "on" if ai.get("model_loaded") else "off", ai.get("avg_infer_ms", 0), present,
            sysd.get("soc_temp_c", 0), sysd.get("mem_available_mb", 0),
            sysd.get("flash_free_mb", -1), sysd.get("flash_total_mb", -1), card, up // 3600, up % 3600 // 60,
            "on" if self.alerts_on else "off")

    def events_text(self):
        _, evs = self.board.events_after(0, 8)
        if not evs:
            return "No events yet."
        lines = []
        for e in reversed(evs[-8:]):
            lines.append("#%d %s %s%s" % (e["id"], format_time(e.get("ts", 0)), EVENT_TITLES.get(e.get("type"), e.get("type")),
                                          " (%s)" % e["label"] if e.get("label") else ""))
        return "\n".join(lines)

    def help_text(self):
        return ("I send an alert with a picture when the camera detects something, and answer:\n"
                "/photo  take a photo\n/video [seconds]  record a short video\n/status  camera status\n"
                "/events  last events\n/alerts on|off  switch alerts\n/menu  buttons\n/whoami  your chat id")

    def handle_action(self, chat, action, arg=""):
        try:
            if action == "photo":
                self.do_photo(chat)
            elif action == "video":
                self.do_video(chat, int(arg) if arg.isdigit() else self.cfg.video_seconds)
            elif action == "status":
                self.tg.send_message(chat, self.status_text())
            elif action == "events":
                self.tg.send_message(chat, self.events_text())
            elif action in ("alerts_on", "alerts_off"):
                self.alerts_on = action == "alerts_on"
                self.tg.send_message(chat, "Alerts are now " + ("ON" if self.alerts_on else "OFF"))
            elif action == "alerts":
                if arg in ("on", "off"):
                    self.handle_action(chat, "alerts_" + arg)
                else:
                    self.tg.send_message(chat, "Alerts are " + ("ON" if self.alerts_on else "OFF") + ". Use /alerts on or /alerts off.")
            elif action in ("menu", "start"):
                self.tg.send_message(chat, "Camera menu", MENU_KEYBOARD)
            elif action == "help":
                self.tg.send_message(chat, self.help_text())
            else:
                self.tg.send_message(chat, "Unknown command. Try /help")
        except (BoardError, NetError, TelegramError, ValueError, KeyError) as e:
            log.warning("command %s failed: %s", action, e)
            try:
                self.tg.send_message(chat, "Sorry, that did not work: %s" % e)
            except (TelegramError, NetError):
                pass

    def handle_update(self, upd):
        cb = upd.get("callback_query")
        if cb:
            chat = cb.get("message", {}).get("chat", {}).get("id")
            self.tg.answer_callback(cb.get("id"))
            if self.authorised(chat):
                self.handle_action(chat, cb.get("data", ""))
            return
        msg = upd.get("message") or {}
        chat = msg.get("chat", {}).get("id")
        text = (msg.get("text") or "").strip()
        if chat is None or not text.startswith("/"):
            return
        parts = text.split()
        cmd = parts[0][1:].split("@")[0].lower()
        arg = parts[1].lower() if len(parts) > 1 else ""
        if cmd == "whoami" or (cmd == "start" and not self.authorised(chat)):
            note = "" if self.authorised(chat) else "\nNot authorised yet: add this id to allowed_chat_ids in the bridge config."
            try:
                self.tg.send_message(chat, "Your chat id is %s%s" % (chat, note))
            except (TelegramError, NetError):
                pass
            return
        if not self.authorised(chat):
            log.warning("ignored a message from unauthorised chat %s", chat)
            return
        self.handle_action(chat, cmd, arg)

    # ---- main loops
    def alert_loop(self):
        delay = self.cfg.poll_interval_s
        while not self.stop.is_set():
            try:
                self.poll_events()
                delay = self.cfg.poll_interval_s
            except (BoardError, NetError, ValueError, KeyError) as e:
                log.warning("board poll failed: %s", e)
                delay = min(delay * 2, 60)
            self.stop.wait(delay)

    def command_loop(self):
        offset = 0
        delay = 1
        while not self.stop.is_set():
            try:
                for upd in self.tg.get_updates(offset):
                    offset = upd["update_id"] + 1
                    self.handle_update(upd)
                delay = 1
            except (TelegramError, NetError) as e:
                log.warning("telegram poll failed: %s", e)
                self.stop.wait(delay)
                delay = min(delay * 2, 60)

    def run(self):
        try:
            self.tg.set_commands(COMMANDS)
        except (TelegramError, NetError) as e:
            log.warning("could not set the command menu: %s", e)
        t = threading.Thread(target=self.alert_loop, daemon=True)
        t.start()
        self.command_loop()


MENU_KEYBOARD = [[{"text": t, "callback_data": d} for t, d in row] for row in MENU]


def main(argv=None):
    ap = argparse.ArgumentParser(description="luckfox-ai Telegram bridge")
    ap.add_argument("--config", default="telegram_bridge.conf")
    ap.add_argument("--download-face-models", action="store_true", help="fetch the two OpenCV face models and exit")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        with open(a.config, "r", encoding="utf-8") as f:
            cfg = parse_config(f.read())
    except (OSError, ConfigError) as e:
        print("config problem: %s" % e, file=sys.stderr)
        return 2
    if a.download_face_models:
        download_face_models(cfg.face_models_dir)
        print("models stored in", cfg.face_models_dir)
        return 0
    namer = None
    if cfg.known_faces_dir:
        try:
            namer = FaceNamer(cfg.known_faces_dir, cfg.face_models_dir, cfg.face_match)
        except Exception as e:
            print("face names switched off: %s\n(pip install opencv-python numpy; python telegram_bridge.py --download-face-models)" % e,
                  file=sys.stderr)
    bridge = Bridge(cfg, Board(cfg.board_url, cfg.board_token), Telegram(cfg.telegram_token, cfg.telegram_api), namer)
    log.info("bridge started for %d chat(s), alerts %s", len(cfg.allowed_chat_ids), "on" if bridge.alerts_on else "off")
    try:
        bridge.run()
    except KeyboardInterrupt:
        bridge.stop.set()
    return 0


if __name__ == "__main__":
    sys.exit(main())
