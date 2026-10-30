"use strict";
// App shell: login, navigation, status polling.
(() => {
  const { el, api, AuthError, toast } = UI;
  const app = document.getElementById("app");
  const ROUTES = [["dashboard", "Dashboard"], ["live", "Live camera"], ["ai", "AI detection"], ["events", "Event history"], ["settings", "Settings"], ["system", "System"]];
  let page = null, content = null, nav = null, pollTimer = null, lastStatus = null, user = "";

  function showLogin(setupRequired) {
    stopAll();
    const u = el("input", { type: "text", value: "admin", autocomplete: "username" });
    const p = el("input", { type: "password", autocomplete: "current-password" });
    const msg = el("div", { class: "note bad hidden" });
    async function go(ev) {
      ev.preventDefault();
      try {
        await api("POST", "/api/login", { user: u.value, password: p.value });
        user = u.value;
        start();
      } catch (e) {
        msg.textContent = e.status === 429 ? "Too many attempts. Wait a minute and try again." : (e.message || "Login failed");
        msg.classList.remove("hidden");
        p.value = "";
      }
    }
    app.replaceChildren(el("div", { class: "login" }, el("form", { class: "card", onsubmit: go },
      el("div", { class: "brand" }, el("img", { src: "/assets/favicon.svg", alt: "" }), el("span", { text: "Luckfox AI Camera" })),
      setupRequired ? el("div", { class: "note warn" }, "No password is set yet. Connect to the board over SSH and run: ", el("span", { class: "mono", text: "/root/aid/aid-core --set-password" })) : null,
      el("div", { class: "field" }, el("label", { text: "User" }), u), el("div", { class: "field" }, el("label", { text: "Password" }), p), msg,
      el("button", { class: "btn primary", type: "submit", text: "Log in" }))));
    p.focus();
  }

  function stopAll() {
    if (page) { try { page.stop(); } catch (e) { /* ignore */ } page = null; }
    clearInterval(pollTimer);
    pollTimer = null;
  }

  function route() {
    const key = (location.hash.replace(/^#\/?/, "") || "dashboard");
    const known = ROUTES.some((r) => r[0] === key) ? key : "dashboard";
    if (page) { try { page.stop(); } catch (e) { /* ignore */ } page = null; }
    for (const a of nav.querySelectorAll("a")) a.classList.toggle("active", a.dataset.route === known);
    content.replaceChildren();
    page = Pages[known](content) || { onStatus() {}, stop() {} };
    if (lastStatus) page.onStatus(lastStatus);
  }

  async function poll() {
    try {
      lastStatus = await api("GET", "/api/status");
      if (page) page.onStatus(lastStatus);
    } catch (e) {
      if (e instanceof AuthError) { showLogin(false); return; }
    }
  }

  function start() {
    stopAll();
    lastStatus = null;
    nav = el("nav", { class: "nav" }, ROUTES.map((r) => el("a", { href: "#/" + r[0], "data-route": r[0], text: r[1] })));
    content = el("main", { class: "main" });
    app.replaceChildren(el("div", { class: "shell" },
      el("aside", { class: "side" }, el("div", { class: "brand" }, el("img", { src: "/assets/favicon.svg", alt: "" }), el("span", { text: "Luckfox AI" })), nav,
        el("div", { class: "foot" }, el("div", { class: "muted", text: user ? "Signed in as " + user : "" }),
          el("button", { class: "btn", text: "Log out", onclick: async () => { try { await api("POST", "/api/logout", {}); } catch (e) { /* ignore */ } showLogin(false); } }))),
      content));
    route();
    poll();
    pollTimer = setInterval(poll, 2000);
  }

  window.addEventListener("hashchange", () => { if (nav) route(); });

  (async () => {
    try {
      const s = await api("GET", "/api/auth/state");
      user = s.user || "";
      if (s.logged_in) start(); else showLogin(s.setup_required);
    } catch (e) {
      app.replaceChildren(el("div", { class: "login" }, el("div", { class: "card" }, el("h2", { text: "Cannot reach the camera" }), el("p", { text: String(e.message || e) }))));
    }
  })();
})();
