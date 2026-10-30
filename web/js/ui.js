"use strict";
// Small DOM helpers, the API wrapper and the live camera view. No libraries.
const UI = (() => {
  function el(tag, attrs, ...kids) {
    const e = document.createElement(tag);
    for (const k in (attrs || {})) {
      const v = attrs[k];
      if (v === null || v === undefined || v === false) continue;
      if (k === "class") e.className = v;
      else if (k === "text") e.textContent = v;
      else if (k.startsWith("on") && typeof v === "function") e.addEventListener(k.slice(2), v);
      else if (k === "value") e.value = v;
      else if (k === "checked") e.checked = !!v;
      else e.setAttribute(k, v === true ? "" : String(v));
    }
    for (const kid of kids.flat()) {
      if (kid === null || kid === undefined || kid === false) continue;
      e.append(kid.nodeType ? kid : document.createTextNode(String(kid)));
    }
    return e;
  }

  class AuthError extends Error {}

  // JSON API call. Throws AuthError on 401, Error(message) on any other failure.
  async function api(method, path, body) {
    const opts = { method, credentials: "same-origin", headers: { "X-Requested-With": "aid-dashboard" } };
    if (body !== undefined) {
      opts.headers["Content-Type"] = "application/json";
      opts.body = JSON.stringify(body);
    }
    const res = await fetch(path, opts);
    let data = null;
    try { data = await res.json(); } catch (e) { /* not JSON */ }
    if (res.status === 401 && !path.startsWith("/api/login")) throw new AuthError("login required");
    if (!res.ok) {
      const err = new Error((data && data.error) || ("HTTP " + res.status));
      err.status = res.status;
      err.data = data;
      throw err;
    }
    return data;
  }

  // Replaces the children of `node` with `content` (a node, text, null, or a nested array of those).
  function fill(node, content) {
    node.replaceChildren(...[].concat(content).flat(Infinity).filter((x) => x !== null && x !== undefined && x !== false));
  }

  let toastTimer = null;
  function toast(msg, kind) {
    const old = document.getElementById("toast");
    if (old) old.remove();
    const t = el("div", { id: "toast", class: "toast " + (kind || ""), text: msg });
    document.body.append(t);
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => t.remove(), 4500);
  }

  const pill = (text, kind) => el("span", { class: "pill " + (kind || ""), text });
  const fmt = (v, d) => (v === null || v === undefined || Number.isNaN(Number(v)) || Number(v) < 0 ? "-" : Number(v).toFixed(d === undefined ? 1 : d));
  const pct = (v) => Math.max(0, Math.min(100, Number(v) || 0));

  function metric(label, valueText, percent, hotAt, critAt) {
    const cls = percent >= (critAt || 101) ? "bar crit" : percent >= (hotAt || 101) ? "bar hot" : "bar";
    const fill = el("span");
    fill.style.width = pct(percent) + "%";
    return el("div", { class: "metric" },
      el("div", { class: "row" }, el("span", { text: label }), el("span", { class: "muted", text: valueText })),
      el("div", { class: cls }, fill));
  }

  function kv(rows) {
    const dl = el("dl", { class: "kv" });
    for (const [k, v] of rows) dl.append(el("dt", { text: k }), el("dd", {}, v));
    return dl;
  }

  function fmtTime(ms) {
    if (!ms) return "-";
    const d = new Date(Number(ms));
    return d.toLocaleDateString() + " " + d.toLocaleTimeString();
  }
  function fmtDuration(s) {
    s = Math.max(0, Math.floor(Number(s) || 0));
    const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
    return (d ? d + "d " : "") + (h ? h + "h " : "") + m + "m";
  }

  // Live camera picture: one request at a time, with detection boxes drawn on a canvas.
  // opts: {interval, boxes (bool), zones (array of [x1,y1,x2,y2]), onZoneDrawn(rect)}
  function liveView(opts) {
    const o = Object.assign({ interval: 700, boxes: true, zones: [] }, opts || {});
    const img = el("img", { alt: "live camera" });
    const canvas = el("canvas");
    const badge = el("div", { class: "badge", text: "connecting..." });
    const root = el("div", { class: "viewer" }, img, canvas, badge);
    let stopped = false, timer = null, dets = [], drag = null, lastDetFetch = 0;

    function layout() {   // where the picture really is inside the (letterboxed) element
      const cw = root.clientWidth, ch = root.clientHeight;
      if (canvas.width !== cw || canvas.height !== ch) { canvas.width = cw; canvas.height = ch; }
      const iw = img.naturalWidth || 16, ih = img.naturalHeight || 9;
      const s = Math.min(cw / iw, ch / ih);
      return { x: (cw - iw * s) / 2, y: (ch - ih * s) / 2, w: iw * s, h: ih * s };
    }
    function draw() {
      const g = canvas.getContext("2d");
      g.clearRect(0, 0, canvas.width, canvas.height);
      const L = layout();
      g.lineWidth = 2;
      g.font = "13px system-ui, sans-serif";
      for (const z of o.zones) {
        g.strokeStyle = "rgba(250, 204, 21, .95)"; g.fillStyle = "rgba(250, 204, 21, .12)";
        g.fillRect(L.x + z[0] * L.w, L.y + z[1] * L.h, (z[2] - z[0]) * L.w, (z[3] - z[1]) * L.h);
        g.strokeRect(L.x + z[0] * L.w, L.y + z[1] * L.h, (z[2] - z[0]) * L.w, (z[3] - z[1]) * L.h);
      }
      if (drag) {
        g.strokeStyle = "#fff"; g.setLineDash([6, 4]);
        g.strokeRect(Math.min(drag.x0, drag.x1), Math.min(drag.y0, drag.y1), Math.abs(drag.x1 - drag.x0), Math.abs(drag.y1 - drag.y0));
        g.setLineDash([]);
      }
      if (!o.boxes) return;
      for (const d of dets) {
        const x = L.x + d.x1 * L.w, y = L.y + d.y1 * L.h, w = (d.x2 - d.x1) * L.w, h = (d.y2 - d.y1) * L.h;
        g.strokeStyle = "#22c55e"; g.strokeRect(x, y, w, h);
        const label = d.label + " " + Math.round(d.conf * 100) + "%";
        const tw = g.measureText(label).width + 8;
        g.fillStyle = "#22c55e"; g.fillRect(x, Math.max(0, y - 18), tw, 18);
        g.fillStyle = "#04130a"; g.fillText(label, x + 4, Math.max(13, y - 5));
      }
    }
    async function fetchDets() {
      if (!o.boxes || Date.now() - lastDetFetch < 900) return;
      lastDetFetch = Date.now();
      try { dets = (await api("GET", "/api/detections")).detections || []; } catch (e) { /* keep old boxes */ }
    }
    function next(delay) { if (!stopped) timer = setTimeout(load, delay); }
    function load() { img.src = "/api/snapshot.jpg?t=" + Date.now(); }
    img.addEventListener("load", () => { badge.textContent = "live"; fetchDets().then(draw); draw(); next(o.interval); });
    img.addEventListener("error", () => { badge.textContent = "no picture"; next(2500); });
    window.addEventListener("resize", draw);

    if (o.onZoneDrawn) {
      const pos = (ev) => { const r = canvas.getBoundingClientRect(); return { x: ev.clientX - r.left, y: ev.clientY - r.top }; };
      canvas.addEventListener("pointerdown", (ev) => { const p = pos(ev); drag = { x0: p.x, y0: p.y, x1: p.x, y1: p.y }; canvas.setPointerCapture(ev.pointerId); });
      canvas.addEventListener("pointermove", (ev) => { if (drag) { const p = pos(ev); drag.x1 = p.x; drag.y1 = p.y; draw(); } });
      canvas.addEventListener("pointerup", () => {
        if (!drag) return;
        const L = layout();
        const cl = (v) => Math.max(0, Math.min(1, v));
        const r = [cl((Math.min(drag.x0, drag.x1) - L.x) / L.w), cl((Math.min(drag.y0, drag.y1) - L.y) / L.h),
                   cl((Math.max(drag.x0, drag.x1) - L.x) / L.w), cl((Math.max(drag.y0, drag.y1) - L.y) / L.h)];
        drag = null;
        draw();
        if (r[2] - r[0] > 0.03 && r[3] - r[1] > 0.03) o.onZoneDrawn(r);
      });
    }
    load();
    return {
      root,
      setZones(z) { o.zones = z; draw(); },
      setBoxes(on) { o.boxes = on; draw(); },
      stop() { stopped = true; clearTimeout(timer); window.removeEventListener("resize", draw); },
    };
  }

  return { el, fill, api, AuthError, toast, pill, fmt, pct, metric, kv, fmtTime, fmtDuration, liveView };
})();
