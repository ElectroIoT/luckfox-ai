"use strict";
// The six dashboard pages. Each builder fills `root` and returns {onStatus(st), stop()}.
const Pages = (() => {
  const { el, fill, api, toast, pill, fmt, pct, metric, kv, fmtTime, fmtDuration, liveView } = UI;
  const CLASSES = ["person", "car", "motorcycle", "bicycle", "bus", "truck", "dog", "cat", "bird", "backpack", "handbag", "suitcase"];
  const EVENT_TYPES = ["person_detected", "vehicle_detected", "animal_detected", "motion_detected", "object_entered",
    "object_disappeared", "camera_disconnected", "camera_reconnected", "system_online", "system_offline", "input_changed"];
  const PRESETS = [["2304x1296 (native)", 2304, 1296], ["1920x1080", 1920, 1080], ["1280x720", 1280, 720], ["960x540", 960, 540]];

  const noop = { onStatus() {}, stop() {} };
  const field = (label, input, hint) => el("div", { class: "field" }, el("label", { text: label }), input, hint ? el("div", { class: "hint", text: hint }) : null);
  const check = (label, checked, onchange) => el("label", { class: "check" }, el("input", { type: "checkbox", checked, onchange }), el("span", { text: label }));
  const num = (v, min, max) => el("input", { type: "number", value: v, min, max });
  const txt = (v, type) => el("input", { type: type || "text", value: v === undefined || v === null ? "" : v, autocomplete: "off" });

  async function saveConfig(patch, restartNote) {
    try {
      const r = await api("POST", "/api/config", patch);
      toast(r.restart_required ? "Saved. Restart the service to apply." : "Saved and applied.", "ok");
      if (r.restart_required && restartNote) restartNote.classList.remove("hidden");
      return r;
    } catch (e) { toast(e.message, "bad"); return null; }
  }
  function restartBox() {
    const box = el("div", { class: "note warn hidden" }, "Some changes need a service restart. ",
      el("button", { class: "btn", text: "Restart service now", onclick: async () => {
        if (!confirm("Restart aid-core now? The camera stream stops for a few seconds.")) return;
        try { await api("POST", "/api/system", { action: "restart_service", confirm: "yes" }); toast("Restarting... reload in a few seconds.", "ok"); } catch (e) { toast(e.message, "bad"); }
      } }));
    return box;
  }

  // ---------------------------------------------------------------- Dashboard
  function dashboard(root) {
    const view = liveView({ interval: 1000, boxes: true });
    const cam = el("div"), ai = el("div"), now = el("div"), sys = el("div"), net = el("div"), sto = el("div");
    root.append(
      el("div", { class: "topbar" }, el("h1", { text: "Dashboard" })),
      el("div", { class: "grid cols-2" },
        el("div", { class: "card" }, el("h2", { text: "Live preview" }), view.root),
        el("div", { class: "grid" },
          el("div", { class: "card" }, el("h2", { text: "Camera" }), cam),
          el("div", { class: "card" }, el("h2", { text: "AI" }), ai))),
      el("div", { class: "grid cols-3", style: null },
        el("div", { class: "card" }, el("h2", { text: "Detected now" }), now),
        el("div", { class: "card" }, el("h2", { text: "System" }), sys),
        el("div", { class: "card" }, el("h2", { text: "Network, storage, MQTT" }), net, sto)));
    return {
      onStatus(st) {
        cam.replaceChildren(kv([
          ["Sensor", st.camera.sensor || "not found"], ["State", pill(st.camera.ok ? "online" : "no frames", st.camera.ok ? "ok" : "bad")],
          ["Resolution", st.camera.width + "x" + st.camera.height], ["Frame rate", fmt(st.stream.recent_fps) + " fps (set " + st.camera.fps_config + ")"],
          ["Bit rate", fmt(st.stream.recent_kbps, 0) + " kbit/s"]]));
        ai.replaceChildren(kv([
          ["Detection", pill(!st.ai.enabled ? "off" : (st.ai.model_loaded ? "running" : "model error"), !st.ai.enabled ? "" : (st.ai.model_loaded ? "ok" : "bad"))],
          ["Inference", fmt(st.ai.avg_infer_ms, 0) + " ms avg"], ["Interval", st.ai.interval_ms + " ms"], ["Confidence", st.ai.conf_pct + " %"],
          ["Motion", pill(st.motion.active ? "motion" : "still", st.motion.active ? "warn" : "")]]));
        const groups = Object.keys(st.present || {});
        now.replaceChildren(groups.length ? el("div", {}, groups.map((g) => el("div", { class: "row" }, pill(g, "ok"), el("span", { text: "x" + st.present[g] })))) : el("div", { class: "muted", text: "Nothing in view." }),
          el("div", { class: "hint", text: "Objects confirmed by the event engine (after the confirmation frames)." }));
        const t = st.system.soc_temp_c;
        sys.replaceChildren(
          metric("Camera program CPU", fmt(st.system.cpu_process_pct, 0) + " %", st.system.cpu_process_pct, 50, 80),
          metric("Memory used", fmt(st.system.mem_total_mb - st.system.mem_available_mb, 0) + " of " + fmt(st.system.mem_total_mb, 0) + " MB", 100 - 100 * st.system.mem_available_mb / st.system.mem_total_mb, 70, 90),
          metric("SoC temperature", fmt(t) + " C", t, 65, 80));
        net.replaceChildren(kv([["Address", st.system.ip || "-"], ["RTSP", el("span", { class: "mono", text: "rtsp://" + (st.system.ip || "<ip>") + ":" + st.stream.rtsp_port + st.stream.rtsp_path })],
          ["MQTT", st.mqtt.enabled ? pill(st.mqtt.connected ? "connected" : "not connected", st.mqtt.connected ? "ok" : "warn") : pill("off")]]));
        sto.replaceChildren(el("div", { class: "hint", text: "Events and snapshots" }),
          st.system.storage_total_mb > 0 ? metric("Free", fmt(st.system.storage_free_mb, 0) + " of " + fmt(st.system.storage_total_mb, 0) + " MB", 100 - 100 * st.system.storage_free_mb / st.system.storage_total_mb, 85, 95) : el("div", { class: "muted", text: "unknown" }));
      },
      stop() { view.stop(); },
    };
  }

  // ---------------------------------------------------------------- Live camera
  function live(root) {
    const view = liveView({ interval: 500, boxes: true });
    const info = el("div");
    const boxes = check("Show detection boxes", true, (e) => view.setBoxes(e.target.checked));
    const sel = el("select", {}, PRESETS.map((p) => el("option", { value: p[1] + "x" + p[2], text: p[0] })));
    const note = restartBox();
    root.append(el("div", { class: "topbar" }, el("h1", { text: "Live camera" })),
      el("div", { class: "card" }, view.root,
        el("div", { class: "row" },
          el("button", { class: "btn primary", text: "Save snapshot", onclick: async () => {
            try {
              const r = await fetch("/api/snapshot.jpg?t=" + Date.now(), { credentials: "same-origin" });
              if (!r.ok) throw new Error("snapshot failed");
              const url = URL.createObjectURL(await r.blob());
              el("a", { href: url, download: "snapshot-" + new Date().toISOString().replace(/[:.]/g, "-") + ".jpg" }).click();
              setTimeout(() => URL.revokeObjectURL(url), 5000);
            } catch (e) { toast(e.message, "bad"); }
          } }), boxes)),
      el("div", { class: "grid cols-2" },
        el("div", { class: "card" }, el("h2", { text: "Stream" }), info,
          el("p", { class: "hint", text: "Play the video in VLC or Home Assistant with the RTSP address above. Recording to a file is not implemented in this version." })),
        el("div", { class: "card" }, el("h2", { text: "Resolution" }),
          field("Camera resolution", sel, "Changing the resolution restarts the camera service."),
          el("button", { class: "btn", text: "Apply", onclick: async () => {
            const [w, h] = sel.value.split("x").map(Number);
            await saveConfig({ width: w, height: h }, note);
          } }), note)));
    return {
      onStatus(st) {
        info.replaceChildren(kv([["Encoded frame rate", fmt(st.stream.recent_fps) + " fps"], ["Bit rate", fmt(st.stream.recent_kbps, 0) + " kbit/s"],
          ["Capture to encoded", fmt(st.stream.encode_latency_avg_ms) + " ms"], ["Key frames", st.stream.keyframes], ["Snapshot time", fmt(st.stream.last_snapshot_ms.total) + " ms"],
          ["RTSP", el("span", { class: "mono", text: "rtsp://" + (st.system.ip || "<ip>") + ":" + st.stream.rtsp_port + st.stream.rtsp_path })]]));
        const cur = st.camera.width + "x" + st.camera.height;
        if (![...sel.options].some((o) => o.value === cur)) sel.append(el("option", { value: cur, text: cur }));
        if (document.activeElement !== sel) sel.value = cur;
      },
      stop() { view.stop(); },
    };
  }

  // ---------------------------------------------------------------- AI detection
  function ai(root) {
    const stateNote = el("div");
    const hist = el("div"), nowBody = el("div");
    let cfg = null, zones = [];
    const view = liveView({ interval: 700, boxes: true, onZoneDrawn(r) { zones.push(r); renderZones(); } });
    const zoneList = el("div", { class: "zone-list" });
    const note = restartBox();
    const classBox = el("div", { class: "checks" });
    const enabled = el("input", { type: "checkbox" });
    const conf = el("input", { type: "range", min: 5, max: 95 }), confLabel = el("span");
    const interval = num(500, 100, 10000);
    const cooldown = num(30, 0, 86400), absent = num(10, 1, 3600), frames = num(2, 1, 20);
    const motion = el("input", { type: "checkbox" }), motionPct = num(3, 1, 100);

    function renderZones() {
      view.setZones(zones);
      fill(zoneList, zones.length ? zones.map((z, i) => el("div", {}, el("span", { class: "mono", text: "zone " + (i + 1) + ": " + z.map((v) => v.toFixed(2)).join(", ") }),
        el("button", { class: "btn danger", text: "Remove", onclick: () => { zones.splice(i, 1); renderZones(); } }))) : el("div", { class: "muted", text: "No zones: the whole picture is watched." }));
    }
    conf.addEventListener("input", () => { confLabel.textContent = conf.value + " %"; });

    async function load() {
      try {
        cfg = await api("GET", "/api/config");
        enabled.checked = cfg.detect_enabled; conf.value = cfg.detect_conf_pct; confLabel.textContent = cfg.detect_conf_pct + " %";
        interval.value = cfg.detect_interval_ms; cooldown.value = cfg.event_cooldown_s; absent.value = cfg.event_absent_s; frames.value = cfg.event_enter_frames;
        motion.checked = cfg.motion_enabled; motionPct.value = cfg.motion_threshold_pct;
        const on = new Set((cfg.detect_classes || "").split(",").filter(Boolean));
        fill(classBox, CLASSES.map((c) => el("label", { class: "check" }, el("input", { type: "checkbox", value: c, checked: on.has(c) }), el("span", { text: c }))));
        zones = (cfg.zone || "").split(";").filter(Boolean).map((s) => s.split(",").map(Number));
        renderZones();
      } catch (e) { toast(e.message, "bad"); }
    }
    async function loadHistory() {
      try {
        const r = await api("GET", "/api/events?limit=15");
        hist.replaceChildren(r.events.length ? el("table", {}, el("thead", {}, el("tr", {}, ["Time", "Event", "Object", "Conf."].map((h) => el("th", { text: h })))),
          el("tbody", {}, r.events.map((e) => el("tr", {}, el("td", { text: fmtTime(e.ts) }), el("td", { text: e.type }), el("td", { text: e.label }), el("td", { text: e.conf ? Math.round(e.conf * 100) + " %" : "" }))))) : el("div", { class: "muted", text: "No events yet." }));
      } catch (e) { /* ignore */ }
    }
    root.append(el("div", { class: "topbar" }, el("h1", { text: "AI detection" })), stateNote,
      el("div", { class: "grid cols-2" },
        el("div", { class: "card" }, el("h2", { text: "Detection zones" }), view.root,
          el("p", { class: "hint", text: "Drag on the picture to add a zone. Events only count objects whose centre is inside a zone." }), zoneList),
        el("div", { class: "card" }, el("h2", { text: "Settings" }),
          el("label", { class: "check" }, enabled, el("span", { text: "Object detection on" })),
          field("Classes to watch", classBox),
          field("Minimum confidence", el("div", { class: "row" }, conf, confLabel)),
          field("Time between detections (ms)", interval, "Lower = more responsive, more CPU/NPU use."),
          field("Confirmation frames", frames, "Detections in a row before an object counts as present."),
          field("Cooldown per event type (s)", cooldown), field("Gone for (s) before 'disappeared'", absent),
          el("label", { class: "check" }, motion, el("span", { text: "Motion detection" })), field("Motion sensitivity: % of picture changed", motionPct),
          el("button", { class: "btn primary", text: "Save", onclick: async () => {
            const classes = [...classBox.querySelectorAll("input:checked")].map((i) => i.value).join(",");
            if (!classes) { toast("Choose at least one class.", "bad"); return; }
            await saveConfig({ detect_enabled: enabled.checked, detect_conf_pct: Number(conf.value), detect_interval_ms: Number(interval.value),
              detect_classes: classes, event_enter_frames: Number(frames.value), event_cooldown_s: Number(cooldown.value), event_absent_s: Number(absent.value),
              motion_enabled: motion.checked, motion_threshold_pct: Number(motionPct.value), zone: zones.map((z) => z.map((v) => v.toFixed(4)).join(",")).join(";") }, note);
          } }), note)),
      el("div", { class: "grid cols-2" },
        el("div", { class: "card" }, el("h2", { text: "In view now" }), nowBody),
        el("div", { class: "card" }, el("h2", { text: "Detection history" }), hist)));
    load(); loadHistory();
    const timer = setInterval(loadHistory, 5000);
    return {
      async onStatus(st) {
        stateNote.replaceChildren(!st.ai.enabled ? el("div", { class: "note", text: "Detection is switched off." }) :
          !st.ai.model_loaded ? el("div", { class: "note bad", text: "The AI model is not loaded: " + (st.ai.error || "unknown error") }) :
          el("div", { class: "note ok", text: "Running: " + st.ai.runs + " detections so far, " + fmt(st.ai.avg_infer_ms, 0) + " ms per inference on the NPU (average)." }));
        try {
          const d = await api("GET", "/api/detections");
          nowBody.replaceChildren(d.detections.length ? el("table", {}, el("tbody", {}, d.detections.map((x) => el("tr", {}, el("td", { text: x.label }), el("td", { text: Math.round(x.conf * 100) + " %" }))))) : el("div", { class: "muted", text: "Nothing detected in the last pass." }));
        } catch (e) { /* ignore */ }
      },
      stop() { view.stop(); clearInterval(timer); },
    };
  }

  // ---------------------------------------------------------------- Events
  function events(root) {
    const list = el("div", { class: "events" }), total = el("span", { class: "muted" });
    const type = el("select", {}, el("option", { value: "", text: "All events" }), EVENT_TYPES.map((t) => el("option", { value: t, text: t })));
    const limit = el("select", {}, [25, 50, 100, 200].map((n) => el("option", { value: n, text: "Show " + n })));
    limit.value = 50;
    async function load() {
      try {
        const r = await api("GET", "/api/events?limit=" + limit.value + (type.value ? "&type=" + encodeURIComponent(type.value) : ""));
        total.textContent = r.total + " stored";
        fill(list, r.events.length ? r.events.map((e) => {
          const thumb = e.snapshot ? el("img", { src: "/api/event_snapshot/" + encodeURIComponent(e.snapshot), alt: e.type, loading: "lazy",
            onclick: () => { const lb = el("div", { class: "lightbox", onclick: () => lb.remove() }, el("img", { src: "/api/event_snapshot/" + encodeURIComponent(e.snapshot), alt: "" })); document.body.append(lb); } }) : el("div", { class: "noimg", text: "no picture" });
          return el("div", { class: "event" }, thumb, el("div", {}, el("div", { class: "t", text: e.type.replaceAll("_", " ") }),
            el("div", { class: "m", text: fmtTime(e.ts) + (e.label ? "  -  " + e.label : "") + (e.conf ? "  -  " + Math.round(e.conf * 100) + " %" : "") + (e.count ? "  -  " + e.count + " in view" : "") }),
            e.detail ? el("div", { class: "m", text: e.detail }) : null));
        }) : el("div", { class: "muted", text: "No events." }));
      } catch (e) { toast(e.message, "bad"); }
    }
    async function remove(query, what) {
      if (!confirm("Delete " + what + "? This cannot be undone.")) return;
      try { const r = await api("DELETE", "/api/events?" + query); toast(r.removed + " events deleted.", "ok"); load(); } catch (e) { toast(e.message, "bad"); }
    }
    type.addEventListener("change", load); limit.addEventListener("change", load);
    root.append(el("div", { class: "topbar" }, el("h1", { text: "Event history" }), total),
      el("div", { class: "card" }, el("div", { class: "row" }, type, limit, el("button", { class: "btn", text: "Refresh", onclick: load }),
        el("a", { class: "btn", href: "/api/events/export?format=csv", text: "Export CSV" }), el("a", { class: "btn", href: "/api/events/export?format=json", text: "Export JSON" }),
        el("button", { class: "btn danger", text: "Delete older than 7 days", onclick: () => remove("before=" + (Date.now() - 7 * 86400000), "events older than 7 days") }),
        el("button", { class: "btn danger", text: "Delete all", onclick: () => remove("all=1", "ALL events and their pictures") }))), list);
    load();
    const timer = setInterval(load, 10000);
    return { onStatus() {}, stop() { clearInterval(timer); } };
  }

  // ---------------------------------------------------------------- Settings
  function settings(root) {
    const wrap = el("div", { class: "grid cols-2" }), note = restartBox();
    root.append(el("div", { class: "topbar" }, el("h1", { text: "Settings" })), note, wrap);
    (async () => {
      let c, sys, mq;
      try { [c, sys, mq] = await Promise.all([api("GET", "/api/config"), api("GET", "/api/system"), api("GET", "/api/mqtt")]); } catch (e) { toast(e.message, "bad"); return; }

      const fps = num(c.fps, 1, 60), bitrate = num(c.bitrate_kbps, 64, 20000), gop = num(c.gop, 1, 600), jq = num(c.jpeg_quality, 1, 99);
      const sl = {}; for (const k of ["brightness", "contrast", "saturation", "sharpness"]) sl[k] = num(c[k], -1, 255);
      wrap.append(el("div", { class: "card" }, el("h2", { text: "Camera and stream" }),
        field("Frame rate", fps), field("Video bit rate (kbit/s)", bitrate), field("Key frame every (frames)", gop), field("Snapshot JPEG quality (1-99)", jq),
        el("p", { class: "hint", text: "Image controls use 0-255; -1 keeps the camera's own value. They apply immediately." }),
        field("Brightness", sl.brightness), field("Contrast", sl.contrast), field("Saturation", sl.saturation), field("Sharpness", sl.sharpness),
        el("button", { class: "btn primary", text: "Save", onclick: () => saveConfig({ fps: +fps.value, bitrate_kbps: +bitrate.value, gop: +gop.value, jpeg_quality: +jq.value,
          brightness: +sl.brightness.value, contrast: +sl.contrast.value, saturation: +sl.saturation.value, sharpness: +sl.sharpness.value }, note) })));

      const rtspPort = num(c.rtsp_port, 1, 65535), rtspPath = txt(c.rtsp_path), httpPort = num(c.http_port, 1, 65535), token = txt("", "password");
      wrap.append(el("div", { class: "card" }, el("h2", { text: "Network" }),
        kv([["Address", sys.ip || "-"], ["Host name", sys.hostname || "-"], ["RTSP", "port " + sys.rtsp_port], ["Web", "port " + sys.http_port]]),
        el("p", { class: "hint", text: "The IP address is set by your router (DHCP). To use a fixed address, reserve it in the router for this board." }),
        field("RTSP port", rtspPort), field("RTSP path", rtspPath), field("Web port", httpPort),
        field("Access token for scripts (snapshot, status)", token, c.http_token_set ? "A token is set. Leave empty to keep it; at least 16 characters to replace it." : "No token set. At least 16 characters."),
        el("button", { class: "btn primary", text: "Save", onclick: () => saveConfig(Object.assign({ rtsp_port: +rtspPort.value, rtsp_path: rtspPath.value, http_port: +httpPort.value }, token.value ? { http_token: token.value } : {}), note) })));

      const m = { en: el("input", { type: "checkbox", checked: c.mqtt_enabled }), host: txt(c.mqtt_host), port: num(c.mqtt_port, 1, 65535), user: txt(c.mqtt_user),
        pass: txt("", "password"), prefix: txt(c.mqtt_prefix), name: txt(c.mqtt_device_name), disc: txt(c.mqtt_discovery_prefix) };
      wrap.append(el("div", { class: "card" }, el("h2", { text: "MQTT / Home Assistant" , }, pill(mq.connected ? "connected" : (mq.enabled ? "not connected" : "off"), mq.connected ? "ok" : "")),
        el("label", { class: "check" }, m.en, el("span", { text: "Enable MQTT" })), field("Broker address", m.host), field("Port", m.port), field("User name", m.user),
        field("Password", m.pass, c.mqtt_password_set ? "A password is stored. Leave empty to keep it." : "No password stored."),
        field("Topic prefix", m.prefix), field("Device name", m.name), field("Discovery prefix", m.disc),
        mq.error ? el("div", { class: "note warn", text: "Last error: " + mq.error }) : null,
        el("p", { class: "hint", text: "MQTT is not encrypted. Use it only on your home network." }),
        el("button", { class: "btn primary", text: "Save", onclick: () => saveConfig(Object.assign({ mqtt_enabled: m.en.checked, mqtt_host: m.host.value, mqtt_port: +m.port.value, mqtt_user: m.user.value,
          mqtt_prefix: m.prefix.value, mqtt_device_name: m.name.value, mqtt_discovery_prefix: m.disc.value }, m.pass.value ? { mqtt_password: m.pass.value } : {}), note) })));

      const keep = num(c.events_keep, 10, 2000), snaps = num(c.snapshots_keep, 0, 500);
      wrap.append(el("div", { class: "card" }, el("h2", { text: "Storage" }),
        kv([["Events and pictures", fmt(sys.data_free_mb, 0) + " of " + fmt(sys.data_total_mb, 0) + " MB free"], ["System flash", fmt(sys.root_free_mb, 0) + " of " + fmt(sys.root_total_mb, 0) + " MB free"]]),
        el("p", { class: "hint", text: "Pictures are written to the board's flash unless you point data_dir at a microSD card in the configuration file. Keep the numbers small to limit flash wear." }),
        field("Events kept", keep), field("Event pictures kept", snaps),
        el("button", { class: "btn primary", text: "Save", onclick: () => saveConfig({ events_keep: +keep.value, snapshots_keep: +snaps.value }, note) })));

      const idle = num(c.session_idle_min, 1, 1440), maxh = num(c.session_max_hours, 1, 168), oldp = txt("", "password"), newp = txt("", "password");
      wrap.append(el("div", { class: "card" }, el("h2", { text: "Security" }),
        field("Log out after inactivity (minutes)", idle), field("Session lifetime (hours)", maxh),
        el("button", { class: "btn", text: "Save", onclick: () => saveConfig({ session_idle_min: +idle.value, session_max_hours: +maxh.value }, note) }),
        el("h3", { text: "Change password" }), field("Current password", oldp), field("New password (8 or more characters)", newp),
        el("button", { class: "btn primary", text: "Change password", onclick: async () => {
          try { await api("POST", "/api/password", { old: oldp.value, new: newp.value }); toast("Password changed. Please log in again.", "ok"); setTimeout(() => location.reload(), 1200); } catch (e) { toast(e.message, "bad"); }
        } })));

      wrap.append(el("div", { class: "card" }, el("h2", { text: "Firmware" }),
        kv([["aid-core", sys.aid_version], ["Board", sys.board || "-"], ["System", sys.os || "-"], ["Kernel", el("span", { class: "mono", text: sys.kernel || "-" })], ["Uptime", fmtDuration(sys.uptime_s)]])));
    })();
    return noop;
  }

  // ---------------------------------------------------------------- System
  function system(root) {
    const cpuHist = [], tempHist = [];
    const metrics = el("div"), chart = el("canvas", { class: "chart", width: 600, height: 140 }), logBox = el("pre", { class: "log" }), info = el("div");
    function draw() {
      const g = chart.getContext("2d"), w = chart.width, h = chart.height;
      g.clearRect(0, 0, w, h);
      const style = getComputedStyle(document.body);
      g.strokeStyle = style.getPropertyValue("--line"); g.strokeRect(0, 0, w, h);
      const line = (arr, max, color) => {
        g.strokeStyle = color; g.beginPath();
        arr.forEach((v, i) => { const x = (i / 59) * w, y = h - Math.min(1, Math.max(0, v / max)) * h; i ? g.lineTo(x, y) : g.moveTo(x, y); });
        g.stroke();
      };
      line(cpuHist, 100, "#38bdf8"); line(tempHist, 100, "#f59e0b");
    }
    async function loadLogs() {
      try { const r = await api("GET", "/api/logs?n=150"); logBox.textContent = r.lines.join("\n"); logBox.scrollTop = logBox.scrollHeight; } catch (e) { /* ignore */ }
    }
    async function act(action, text) {
      if (!confirm(text)) return;
      try { await api("POST", "/api/system", { action, confirm: "yes" }); toast("Done: " + action.replace("_", " ") + ".", "ok"); } catch (e) { toast(e.message, "bad"); }
    }
    api("GET", "/api/system").then((s) => info.replaceChildren(kv([["Version", s.aid_version], ["Board", s.board || "-"], ["Kernel", el("span", { class: "mono", text: s.kernel || "-" })], ["Host name", s.hostname || "-"],
      ["Address", s.ip || "-"], ["Uptime", fmtDuration(s.uptime_s)]]))).catch(() => {});
    root.append(el("div", { class: "topbar" }, el("h1", { text: "System" })),
      el("div", { class: "grid cols-2" },
        el("div", { class: "card" }, el("h2", { text: "Load" }), metrics, chart, el("div", { class: "hint", text: "Blue: whole-system CPU % (approximate). Orange: SoC temperature (C). Last 60 samples, 2 s apart." })),
        el("div", { class: "card" }, el("h2", { text: "Board" }), info,
          el("div", { class: "row" },
            el("button", { class: "btn", text: "Restart service", onclick: () => act("restart_service", "Restart aid-core? The camera stream stops for a few seconds.") }),
            el("button", { class: "btn danger", text: "Reboot", onclick: () => act("reboot", "Reboot the whole board now?") }),
            el("button", { class: "btn danger", text: "Shut down", onclick: () => act("shutdown", "Shut the board down? You must unplug and replug the power to start it again.") })))),
      el("div", { class: "card" }, el("h2", { text: "Application log" }), logBox));
    loadLogs();
    const timer = setInterval(loadLogs, 3000);
    return {
      onStatus(st) {
        cpuHist.push(Math.max(0, st.system.cpu_system_pct)); tempHist.push(Math.max(0, st.system.soc_temp_c));
        if (cpuHist.length > 60) { cpuHist.shift(); tempHist.shift(); }
        draw();
        metrics.replaceChildren(metric("aid-core CPU (of one core)", fmt(st.system.cpu_process_pct, 0) + " %", st.system.cpu_process_pct, 50, 80),
          metric("Whole system CPU (approximate, reads low)", fmt(st.system.cpu_system_pct, 0) + " %", st.system.cpu_system_pct, 70, 90),
          metric("Memory used", fmt(st.system.mem_total_mb - st.system.mem_available_mb, 0) + " of " + fmt(st.system.mem_total_mb, 0) + " MB", 100 - 100 * st.system.mem_available_mb / st.system.mem_total_mb, 70, 90),
          metric("aid-core memory", fmt(st.system.process_rss_mb) + " MB", 100 * st.system.process_rss_mb / st.system.mem_total_mb, 40, 60),
          metric("SoC temperature", fmt(st.system.soc_temp_c) + " C", st.system.soc_temp_c, 65, 80));
      },
      stop() { clearInterval(timer); },
    };
  }

  return { dashboard, live, ai, events, settings, system };
})();
