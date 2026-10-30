"""PC tests for tools/telegram_bridge.py. No network: the board and Telegram are faked.
Run: python3 -m unittest tests/test_telegram_bridge.py   (scripts/test.sh does it)
"""
import http.server
import json
import os
import subprocess
import sys
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import telegram_bridge as tb  # noqa: E402

GOOD = """
# comment
board_url=http://192.168.1.160:8080/
board_token=tok123
telegram_token=123456:ABC-def
allowed_chat_ids=111, 222
"""


def cfg(extra=""):
    return tb.parse_config(GOOD + extra)


class FakeBoard(object):
    """Stands in for tb.Board."""

    def __init__(self):
        self.events = []
        self.last_id = 0
        self.snap = b"LIVE"
        self.fail_capture = False
        self.captures = 0
        self.status_json = {"uptime_s": 7260, "camera": {"ok": True}, "stream": {"recent_fps": 28.5},
                            "ai": {"model_loaded": True, "avg_infer_ms": 89},
                            "present": {"person": 1},
                            "system": {"soc_temp_c": 65, "mem_available_mb": 27.4, "flash_free_mb": 130, "flash_total_mb": 181,
                                       "sd": {"present": True, "in_use": True, "free_mb": 900, "total_mb": 1900}}}

    def add(self, typ, snapshot="x.jpg", **kw):
        self.last_id += 1
        e = {"id": self.last_id, "ts": 1700000000000, "type": typ, "label": kw.get("label", "person"), "conf": 0.87, "snapshot": snapshot}
        self.events.append(e)
        return e

    def events_after(self, after_id, limit=50):
        evs = [e for e in self.events if e["id"] > after_id][-limit:]
        return self.last_id, evs

    def event_snapshot(self, name):
        return b"EVENT:" + name.encode()

    def snapshot(self):
        return self.snap

    def capture(self):
        if self.fail_capture:
            raise tb.BoardError("storage full")
        self.captures += 1
        return b"CAPTURE"

    def status(self):
        return self.status_json


class FakeTelegram(object):
    def __init__(self):
        self.sent = []          # (kind, chat, payload, caption)
        self.fail_chats = set()

    def send_message(self, chat, text, keyboard=None):
        if chat in self.fail_chats:
            raise tb.TelegramError("blocked")
        self.sent.append(("text", chat, text, keyboard))

    def send_photo(self, chat, jpeg, caption=""):
        if chat in self.fail_chats:
            raise tb.TelegramError("blocked")
        self.sent.append(("photo", chat, jpeg, caption))

    def send_video(self, chat, mp4, caption=""):
        self.sent.append(("video", chat, mp4, caption))

    def answer_callback(self, cid):
        self.sent.append(("answer", cid, None, None))

    def kinds(self):
        return [s[0] for s in self.sent]


class FakeNamer(object):
    def __init__(self, results):
        self.results = list(results)   # one answer per identify() call; last one repeats
        self.calls = []

    def identify(self, jpeg):
        self.calls.append(jpeg)
        return self.results.pop(0) if len(self.results) > 1 else self.results[0]


def make(extra="", namer=None, run=None):
    sleeps = []
    b, t = FakeBoard(), FakeTelegram()
    br = tb.Bridge(cfg(extra), b, t, namer, run=run or subprocess.run, sleep=sleeps.append)
    return br, b, t, sleeps


def msg(chat, text):
    return {"update_id": 1, "message": {"chat": {"id": chat}, "text": text}}


class ConfigTests(unittest.TestCase):
    def test_good(self):
        c = cfg("alert_types=person_detected,vehicle_detected\nvideo_seconds=15\n")
        self.assertEqual(c.board_url, "http://192.168.1.160:8080")
        self.assertEqual(c.allowed_chat_ids, [111, 222])
        self.assertEqual(c.alert_types, ["person_detected", "vehicle_detected"])
        self.assertEqual(c.rtsp_url, "rtsp://192.168.1.160:554/live/0")
        self.assertEqual(c.video_seconds, 15)

    def test_bad(self):
        bad = ["board_url=ftp://x\n", "board_token=\n", "telegram_token=nocolon\n", "allowed_chat_ids=\n",
               "allowed_chat_ids=abc\n", "video_seconds=500\n", "poll_interval_s=0\n", "nonsense=1\n",
               "alert_types=Person Detected\n", "alerts_enabled=maybe\n", "face_match=5\n"]
        for extra in bad:
            text = GOOD
            key = extra.split("=")[0]
            text = "\n".join(l for l in text.splitlines() if not l.startswith(key + "=")) + "\n" + extra
            with self.assertRaises(tb.ConfigError, msg=extra):
                tb.parse_config(text)


class AlertTests(unittest.TestCase):
    def test_first_poll_skips_history_then_alerts(self):
        br, b, t, _ = make()
        b.add("person_detected")
        b.add("person_detected")
        self.assertEqual(br.poll_events(), 0)               # history is not replayed
        self.assertEqual(br.cursor, 2)
        b.add("motion_detected")                            # not in alert_types
        b.add("person_detected", snapshot="7.jpg")
        self.assertEqual(br.poll_events(), 1)
        self.assertEqual(t.kinds(), ["photo", "photo"])     # both allowed chats
        self.assertEqual({s[1] for s in t.sent}, {111, 222})
        self.assertEqual(t.sent[0][2], b"EVENT:7.jpg")
        self.assertIn("Person detected (87%)", t.sent[0][3])
        self.assertEqual(br.poll_events(), 0)               # nothing twice

    def test_alerts_off_still_advances(self):
        br, b, t, _ = make()
        br.poll_events()
        br.alerts_on = False
        b.add("person_detected")
        self.assertEqual(br.poll_events(), 0)
        br.alerts_on = True
        self.assertEqual(br.poll_events(), 0)               # the missed one is not sent later
        self.assertEqual(t.sent, [])

    def test_no_snapshot_sends_text(self):
        br, b, t, _ = make()
        br.poll_events()
        b.add("person_detected", snapshot="")
        br.poll_events()
        self.assertEqual(t.kinds(), ["text", "text"])

    def test_board_restart_resets_numbering(self):
        br, b, t, _ = make()
        for _ in range(5):
            b.add("motion_detected")
        br.poll_events()
        self.assertEqual(br.cursor, 5)
        b.events, b.last_id = [], 0                         # board rebooted, history gone
        b.add("person_detected")                            # new #1
        self.assertEqual(br.poll_events(), 1)
        self.assertEqual(br.cursor, 1)

    def test_one_blocked_chat_does_not_stop_the_other(self):
        br, b, t, _ = make()
        br.poll_events()
        t.fail_chats.add(111)
        b.add("person_detected")
        br.poll_events()
        self.assertEqual([s[1] for s in t.sent], [222])

    def test_event_picture_missing_falls_back_to_text(self):
        br, b, t, _ = make()
        br.poll_events()
        b.event_snapshot = lambda n: (_ for _ in ()).throw(tb.BoardError("gone"))
        b.add("person_detected")
        br.poll_events()
        self.assertEqual(t.kinds(), ["text", "text"])


class NameTests(unittest.TestCase):
    def test_known_person_named(self):
        br, b, t, _ = make(namer=FakeNamer([[("Ravi", 0.8)]]))
        br.poll_events()
        b.add("person_detected")
        br.poll_events()
        self.assertIn("Ravi detected", t.sent[0][3])

    def test_two_known_and_unknown(self):
        br, b, t, _ = make(namer=FakeNamer([[("Ravi", 0.8), ("Priya", 0.7), (None, 0.1)]]))
        br.poll_events()
        b.add("person_detected")
        br.poll_events()
        self.assertIn("Ravi, Priya and an unknown person detected", t.sent[0][3])

    def test_unknown_only(self):
        br, b, t, _ = make(namer=FakeNamer([[(None, 0.2)]]))
        br.poll_events()
        b.add("person_detected")
        br.poll_events()
        self.assertIn("an unknown person detected", t.sent[0][3])

    def test_no_face_then_live_picture_with_face(self):
        n = FakeNamer([[], [("Ravi", 0.9)]])
        br, b, t, sleeps = make(namer=n)
        br.poll_events()
        b.add("person_detected")
        br.poll_events()
        self.assertEqual(sleeps, [1])                       # waited one second, tried a live picture
        self.assertEqual(n.calls, [b"EVENT:x.jpg", b"LIVE"])
        self.assertEqual(t.sent[0][2], b"LIVE")             # the picture that shows the face
        self.assertIn("Ravi detected", t.sent[0][3])

    def test_no_face_ever_gives_plain_alert(self):
        n = FakeNamer([[]])
        br, b, t, sleeps = make("recognise_burst=2\n", namer=n)
        br.poll_events()
        b.add("person_detected")
        br.poll_events()
        self.assertEqual(len(sleeps), 2)
        self.assertEqual(t.sent[0][2], b"EVENT:x.jpg")
        self.assertIn("Person detected", t.sent[0][3])

    def test_face_code_crash_does_not_block_alert(self):
        class Boom(object):
            def identify(self, jpeg):
                raise RuntimeError("opencv exploded")
        br, b, t, _ = make(namer=Boom())
        br.poll_events()
        b.add("person_detected")
        self.assertEqual(br.poll_events(), 1)
        self.assertEqual(t.kinds(), ["photo", "photo"])

    def test_vehicle_events_are_not_named(self):
        n = FakeNamer([[("Ravi", 0.9)]])
        br, b, t, _ = make("alert_types=vehicle_detected\n", namer=n)
        br.poll_events()
        b.add("vehicle_detected", label="car")
        br.poll_events()
        self.assertEqual(n.calls, [])
        self.assertIn("Vehicle detected", t.sent[0][3])

    def test_matching_math(self):
        self.assertAlmostEqual(tb.cosine([1, 0], [1, 0]), 1.0)
        self.assertAlmostEqual(tb.cosine([1, 0], [0, 1]), 0.0)
        self.assertEqual(tb.cosine([0, 0], [1, 1]), 0.0)
        gallery = {"Ravi": [[1, 0, 0], [0.9, 0.1, 0]], "Priya": [[0, 1, 0]]}
        self.assertEqual(tb.match_name([0.95, 0.05, 0], gallery, 0.5)[0], "Ravi")
        self.assertEqual(tb.match_name([0, 0.9, 0.1], gallery, 0.5)[0], "Priya")
        self.assertIsNone(tb.match_name([0, 0, 1], gallery, 0.5)[0])        # a stranger
        self.assertIsNone(tb.match_name([1, 0, 0], {}, 0.5)[0])             # nobody enrolled


class CommandTests(unittest.TestCase):
    def test_unauthorised_chat_is_ignored(self):
        br, b, t, _ = make()
        for text in ("/photo", "/video", "/status", "/events", "/alerts off", "/menu"):
            br.handle_update(msg(999, text))
        self.assertEqual(t.sent, [])
        self.assertTrue(br.alerts_on)

    def test_whoami_and_start_for_strangers(self):
        br, b, t, _ = make()
        br.handle_update(msg(999, "/whoami"))
        br.handle_update(msg(999, "/start"))
        self.assertEqual(len(t.sent), 2)
        self.assertIn("999", t.sent[0][2])
        self.assertIn("Not authorised", t.sent[1][2])
        self.assertNotIn("tok123", repr(t.sent))

    def test_photo_is_stored_on_board(self):
        br, b, t, _ = make()
        br.handle_update(msg(111, "/photo"))
        self.assertEqual(b.captures, 1)
        self.assertEqual((t.sent[0][0], t.sent[0][2]), ("photo", b"CAPTURE"))

    def test_photo_falls_back_to_live_picture(self):
        br, b, t, _ = make()
        b.fail_capture = True
        br.handle_update(msg(111, "/photo"))
        self.assertEqual(t.sent[0][2], b"LIVE")

    def test_photo_without_storing(self):
        br, b, t, _ = make("save_photos_on_board=0\n")
        br.handle_update(msg(111, "/photo"))
        self.assertEqual(b.captures, 0)
        self.assertEqual(t.sent[0][2], b"LIVE")

    def test_bot_name_suffix_and_case(self):
        br, b, t, _ = make()
        br.handle_update(msg(111, "/Photo@my_camera_bot"))
        self.assertEqual(t.sent[0][0], "photo")

    def test_alerts_toggle(self):
        br, b, t, _ = make()
        br.handle_update(msg(111, "/alerts off"))
        self.assertFalse(br.alerts_on)
        br.handle_update(msg(111, "/alerts on"))
        self.assertTrue(br.alerts_on)
        br.handle_update(msg(111, "/alerts"))
        self.assertIn("Use /alerts on", t.sent[-1][2])

    def test_menu_and_callbacks(self):
        br, b, t, _ = make()
        br.handle_update(msg(111, "/menu"))
        kb = t.sent[0][3]
        self.assertEqual([btn["callback_data"] for row in kb for btn in row],
                         ["photo", "video", "status", "events", "alerts_on", "alerts_off"])
        br.handle_update({"update_id": 2, "callback_query": {"id": "c1", "data": "alerts_off", "message": {"chat": {"id": 111}}}})
        self.assertFalse(br.alerts_on)
        br.handle_update({"update_id": 3, "callback_query": {"id": "c2", "data": "alerts_off", "message": {"chat": {"id": 999}}}})
        br.handle_update({"update_id": 4, "callback_query": {"id": "c3", "data": "alerts_on", "message": {"chat": {"id": 999}}}})
        self.assertFalse(br.alerts_on)                      # a stranger's button press did nothing

    def test_status_and_events(self):
        br, b, t, _ = make()
        b.add("person_detected")
        b.add("motion_detected", label="")
        br.handle_update(msg(111, "/status"))
        text = t.sent[0][2]
        self.assertIn("28 fps", text.replace("28.5", "28"))
        self.assertIn("130 of 181 MB", text)
        self.assertIn("card 900 of 1900 MB free", text)
        self.assertIn("2h 01m", text)
        br.handle_update(msg(111, "/events"))
        self.assertIn("#2", t.sent[1][2])
        self.assertIn("Motion detected", t.sent[1][2])

    def test_board_down_is_reported_not_raised(self):
        br, b, t, _ = make()
        b.status = lambda: (_ for _ in ()).throw(tb.NetError("timed out"))
        br.handle_update(msg(111, "/status"))
        self.assertIn("did not work", t.sent[0][2])

    def test_video(self):
        calls = []

        def fake_run(cmd, **kw):
            calls.append(cmd)
            with open(cmd[-1], "wb") as f:
                f.write(b"MP4DATA")
            return subprocess.CompletedProcess(cmd, 0, b"", b"")
        br, b, t, _ = make(run=fake_run)
        br.handle_update(msg(111, "/video 12"))
        self.assertEqual(t.kinds(), ["text", "video"])
        self.assertEqual(t.sent[1][2], b"MP4DATA")
        cmd = calls[0]
        self.assertEqual(cmd[cmd.index("-t") + 1], "12")
        self.assertIn("rtsp://192.168.1.160:554/live/0", cmd)
        self.assertFalse(os.path.exists(os.path.dirname(cmd[-1])))      # temp dir removed

    def test_video_length_is_limited(self):
        calls = []

        def fake_run(cmd, **kw):
            calls.append(cmd)
            with open(cmd[-1], "wb") as f:
                f.write(b"x")
            return subprocess.CompletedProcess(cmd, 0, b"", b"")
        br, b, t, _ = make(run=fake_run)
        br.handle_update(msg(111, "/video 9999"))
        self.assertEqual(calls[0][calls[0].index("-t") + 1], "60")
        br.handle_update(msg(111, "/video"))
        self.assertEqual(calls[1][calls[1].index("-t") + 1], "10")      # default video_seconds

    def test_video_ffmpeg_missing_or_failing(self):
        def missing(cmd, **kw):
            raise FileNotFoundError("ffmpeg")
        br, b, t, _ = make(run=missing)
        br.handle_update(msg(111, "/video"))
        self.assertIn("ffmpeg was not found", t.sent[-1][2])

        def failing(cmd, **kw):
            return subprocess.CompletedProcess(cmd, 1, b"", b"Connection refused")
        br, b, t, _ = make(run=failing)
        br.handle_update(msg(111, "/video"))
        self.assertIn("ffmpeg failed", t.sent[-1][2])

    def test_unknown_command(self):
        br, b, t, _ = make()
        br.handle_update(msg(111, "/rm -rf"))
        self.assertIn("Unknown command", t.sent[0][2])
        br.handle_update(msg(111, "hello"))                 # plain text is ignored
        self.assertEqual(len(t.sent), 1)


class WireTests(unittest.TestCase):
    """Board and Telegram wrappers against fake request functions, plus real HTTP plumbing."""

    def test_board_calls(self):
        seen = []

        def fake(url, method="GET", headers=None, data=None, timeout=15):
            seen.append((url, method, headers))
            if "/api/events" in url:
                return 200, json.dumps({"last_id": 9, "events": [{"id": 9}, {"id": 8}]}).encode()
            if url.endswith("/api/capture"):
                return 200, b'{"ok":true,"name":"123.jpg"}'
            if "/api/capture/" in url:
                return 200, b"JPG"
            return 401, b"{}"
        b = tb.Board("http://x:8080", "secret", fake)
        last, evs = b.events_after(5, 20)
        self.assertEqual((last, [e["id"] for e in evs]), (9, [8, 9]))     # oldest first
        self.assertIn("after_id=5", seen[0][0])
        self.assertEqual(seen[0][2]["Authorization"], "Bearer secret")
        self.assertEqual(b.capture(), b"JPG")
        self.assertEqual(seen[1][1], "POST")
        self.assertEqual(seen[1][2]["X-Requested-With"], "aid-dashboard")
        with self.assertRaises(tb.BoardError) as cm:
            b.snapshot()
        self.assertIn("refused the token", str(cm.exception))
        self.assertNotIn("secret", str(cm.exception))

    def test_telegram_errors(self):
        def fake(url, method="GET", headers=None, data=None, timeout=15):
            return 401, b'{"ok":false,"description":"Unauthorized"}'
        t = tb.Telegram("1:SECRET", request=fake)
        with self.assertRaises(tb.TelegramError) as cm:
            t.send_message(1, "x")
        self.assertIn("Unauthorized", str(cm.exception))
        self.assertNotIn("SECRET", str(cm.exception))

        def html(url, method="GET", headers=None, data=None, timeout=15):
            return 502, b"<html>bad gateway</html>"
        with self.assertRaises(tb.TelegramError):
            tb.Telegram("1:SECRET", request=html).send_message(1, "x")

    def test_multipart_and_real_http(self):
        got = {}

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_POST(self):
                n = int(self.headers.get("Content-Length", 0))
                got["body"] = self.rfile.read(n)
                got["ctype"] = self.headers.get("Content-Type")
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'{"ok":true,"result":{"message_id":1}}')

        srv = http.server.HTTPServer(("127.0.0.1", 0), H)
        th = threading.Thread(target=srv.serve_forever, daemon=True)
        th.start()
        try:
            api = "http://127.0.0.1:%d" % srv.server_address[1]
            t = tb.Telegram("42:TOKEN", api)
            res = t.send_photo(111, b"\xff\xd8JPEGDATA", "caption ✓")
            self.assertEqual(res["message_id"], 1)
            self.assertIn("multipart/form-data; boundary=", got["ctype"])
            self.assertIn(b'name="chat_id"\r\n\r\n111', got["body"])
            self.assertIn(b"JPEGDATA", got["body"])
            self.assertIn("caption ✓".encode("utf-8"), got["body"])
        finally:
            srv.shutdown()
        # nothing listening: a clean NetError without the URL in the message
        with self.assertRaises(tb.NetError) as cm:
            tb.http_request("http://127.0.0.1:1/botSECRET/x", timeout=2)
        self.assertNotIn("SECRET", str(cm.exception))


class LoopTests(unittest.TestCase):
    def test_command_loop_handles_updates_and_survives_errors(self):
        br, b, t, _ = make()
        calls = {"n": 0}

        def get_updates(offset, wait=25):
            calls["n"] += 1
            if calls["n"] == 1:
                raise tb.NetError("offline")
            if calls["n"] == 2:
                return [msg(111, "/alerts off")]
            br.stop.set()
            return []
        t.get_updates = get_updates
        br.stop.wait = lambda d: None
        br.command_loop()
        self.assertFalse(br.alerts_on)
        self.assertEqual(calls["n"], 3)


if __name__ == "__main__":
    unittest.main()
