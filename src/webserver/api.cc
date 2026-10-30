#include "api.h"

#include <time.h>

#include <cstdlib>
#include <map>
#include <sstream>

#include "json_util.h"
#include "log.h"

namespace aid {

namespace {

int64_t realNowMs() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

std::string b(bool v) { return v ? "true" : "false"; }
std::string q(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }

std::string joinCsv(const std::vector<std::string>& v) {
    std::string o;
    for (size_t i = 0; i < v.size(); ++i) o += (i ? "," : "") + v[i];
    return o;
}

bool parseLong(const std::string& s, long long* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const long long v = strtoll(s.c_str(), &end, 10);
    if (*end != '\0') return false;
    *out = v;
    return true;
}

// Settings the dashboard may change. Paths (data_dir, web_dir, auth_file, iq_dir, model/labels
// paths) are deliberately not editable over the network.
const char* kEditableKeys[] = {
    "width", "height", "fps", "bitrate_kbps", "gop", "jpeg_quality", "brightness", "contrast",
    "saturation", "sharpness", "rtsp_port", "rtsp_path", "http_port", "http_token", "detect_enabled",
    "detect_interval_ms", "detect_conf_pct", "detect_classes", "zone", "event_cooldown_s",
    "event_absent_s", "event_enter_frames", "motion_enabled", "motion_threshold_pct", "events_keep",
    "snapshots_keep", "mqtt_enabled", "mqtt_host", "mqtt_port", "mqtt_user", "mqtt_password",
    "mqtt_prefix", "mqtt_device_name", "mqtt_discovery_prefix", "gpio_enabled", "gpio",
    "session_idle_min", "session_max_hours"};

bool editable(const std::string& k) {
    for (const char* e : kEditableKeys)
        if (k == e) return true;
    return false;
}

// Settings that only take effect after a restart of aid-core.
bool needsRestart(const Config& a, const Config& c) {
    return a.width != c.width || a.height != c.height || a.fps != c.fps || a.bitrateKbps != c.bitrateKbps ||
           a.gop != c.gop || a.jpegQuality != c.jpegQuality || a.rtspPort != c.rtspPort ||
           a.rtspPath != c.rtspPath || a.httpPort != c.httpPort || a.httpToken != c.httpToken ||
           a.mqttEnabled != c.mqttEnabled || a.mqttHost != c.mqttHost || a.mqttPort != c.mqttPort ||
           a.mqttUser != c.mqttUser || a.mqttPassword != c.mqttPassword || a.mqttPrefix != c.mqttPrefix ||
           a.mqttDeviceName != c.mqttDeviceName || a.mqttDiscoveryPrefix != c.mqttDiscoveryPrefix ||
           a.gpioEnabled != c.gpioEnabled || a.sessionIdleMin != c.sessionIdleMin ||
           a.sessionMaxHours != c.sessionMaxHours || a.eventsKeep != c.eventsKeep ||
           a.snapshotsKeep != c.snapshotsKeep || a.gpio.size() != c.gpio.size();
}

}  // namespace

Api::Api(AuthManager* auth, EventStore* events, const std::string& webDir, const std::string& machineToken,
         int sessionMaxHours, ApiBackend backend)
    : auth_(auth), events_(events), webDir_(webDir), token_(machineToken), maxHours_(sessionMaxHours),
      be_(std::move(backend)) {}

void Api::json(HttpResponse* r, int status, const std::string& body) const {
    setText(r, status, body, "application/json");
}

void Api::error(HttpResponse* r, int status, const std::string& msg) const {
    json(r, status, "{\"error\":" + q(msg) + "}");
}

std::string Api::configJson(const Config& c) const {
    std::ostringstream o;
    o << "{";
    o << "\"width\":" << c.width << ",\"height\":" << c.height << ",\"fps\":" << c.fps
      << ",\"bitrate_kbps\":" << c.bitrateKbps << ",\"gop\":" << c.gop << ",\"jpeg_quality\":" << c.jpegQuality;
    o << ",\"brightness\":" << c.brightness << ",\"contrast\":" << c.contrast << ",\"saturation\":" << c.saturation
      << ",\"sharpness\":" << c.sharpness;
    o << ",\"rtsp_port\":" << c.rtspPort << ",\"rtsp_path\":" << q(c.rtspPath) << ",\"http_port\":" << c.httpPort
      << ",\"http_token_set\":" << b(!c.httpToken.empty());
    o << ",\"detect_enabled\":" << b(c.detectEnabled) << ",\"detect_interval_ms\":" << c.detectIntervalMs
      << ",\"detect_conf_pct\":" << c.detectConfPct << ",\"detect_classes\":" << q(joinCsv(c.detectClasses));
    std::string zones;
    for (size_t i = 0; i < c.zones.size(); ++i) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%.3f,%.3f,%.3f,%.3f", c.zones[i].x1, c.zones[i].y1, c.zones[i].x2, c.zones[i].y2);
        zones += (i ? ";" : "") + std::string(buf);
    }
    o << ",\"zone\":" << q(zones);
    o << ",\"event_cooldown_s\":" << c.eventCooldownS << ",\"event_absent_s\":" << c.eventAbsentS
      << ",\"event_enter_frames\":" << c.eventEnterFrames << ",\"motion_enabled\":" << b(c.motionEnabled)
      << ",\"motion_threshold_pct\":" << c.motionThresholdPct << ",\"events_keep\":" << c.eventsKeep
      << ",\"snapshots_keep\":" << c.snapshotsKeep;
    o << ",\"mqtt_enabled\":" << b(c.mqttEnabled) << ",\"mqtt_host\":" << q(c.mqttHost) << ",\"mqtt_port\":" << c.mqttPort
      << ",\"mqtt_user\":" << q(c.mqttUser) << ",\"mqtt_password_set\":" << b(!c.mqttPassword.empty())
      << ",\"mqtt_prefix\":" << q(c.mqttPrefix) << ",\"mqtt_device_name\":" << q(c.mqttDeviceName)
      << ",\"mqtt_discovery_prefix\":" << q(c.mqttDiscoveryPrefix);
    std::string gp;
    for (size_t i = 0; i < c.gpio.size(); ++i)
        gp += (i ? ";" : "") + c.gpio[i].name + "," + c.gpio[i].role + "," + std::to_string(c.gpio[i].pin) + "," +
              (c.gpio[i].activeLow ? "1" : "0");
    o << ",\"gpio_enabled\":" << b(c.gpioEnabled) << ",\"gpio\":" << q(gp);
    o << ",\"session_idle_min\":" << c.sessionIdleMin << ",\"session_max_hours\":" << c.sessionMaxHours;
    o << "}";
    return o.str();
}

bool Api::applyConfigPatch(const std::string& body, std::string* err, bool* restartRequired) {
    std::map<std::string, std::string> kv;
    if (!jsonParseFlat(body, &kv)) { *err = "the request body is not a flat JSON object"; return false; }
    if (kv.empty()) { *err = "no settings given"; return false; }

    const Config current = be_.getConfig();
    Config next = current;
    std::string text;
    bool clearZones = false, clearGpio = false;
    for (const auto& e : kv) {
        const std::string& k = e.first;
        std::string v = e.second;
        if (!editable(k)) { *err = "setting '" + k + "' cannot be changed here"; return false; }
        if ((k == "http_token" || k == "mqtt_password") && v.empty()) continue;   // empty = keep the secret
        for (char c : v)
            if (c == '\n' || c == '\r' || c == '#') { *err = "invalid character in '" + k + "'"; return false; }
        if (k == "zone" || k == "gpio") {
            if (v.empty()) { (k == "zone" ? clearZones : clearGpio) = true; continue; }
            std::string cur;
            for (size_t i = 0; i <= v.size(); ++i) {
                if (i == v.size() || v[i] == ';') { if (!cur.empty()) text += k + "=" + cur + "\n"; cur.clear(); }
                else cur += v[i];
            }
        } else {
            text += k + "=" + v + "\n";
        }
    }
    if (clearZones) next.zones.clear();
    if (clearGpio) next.gpio.clear();
    std::vector<std::string> problems;
    if (!parseConfig(text, &next, &problems)) {
        *err = problems.empty() ? "invalid value" : problems[0];
        return false;
    }
    if (!problems.empty()) { *err = problems[0]; return false; }   // unparsable values are errors here
    *restartRequired = needsRestart(current, next);
    return be_.applyConfig(next, err, restartRequired);
}

bool Api::handle(const HttpRequest& req, HttpResponse* resp, int64_t nowMs) {
    if (nowMs == 0) nowMs = realNowMs();
    const bool isApi = req.path.compare(0, 5, "/api/") == 0;

    if (req.path == "/health" && req.method == "GET") { setText(resp, 200, "ok\n"); return true; }

    const std::string sessionToken = cookieValue(req.cookie, "aid_session");
    const bool session = auth_->validate(sessionToken, nowMs);

    if (req.method == "GET" && (req.path == "/snapshot.jpg" || req.path == "/status.json")) {
        if (!session && !tokenAuthorized(req, token_)) { error(resp, 401, "login or token required"); return true; }
        if (req.path == "/status.json") { json(resp, 200, be_.statusJson ? be_.statusJson() : "{}"); return true; }
        std::vector<uint8_t> jpeg;
        std::string e;
        if (be_.snapshot && be_.snapshot(&jpeg, &e)) { resp->contentType = "image/jpeg"; resp->body = jpeg; }
        else error(resp, 503, "snapshot failed: " + e);
        return true;
    }

    if (!isApi) {
        if (req.method == "GET" && serveStaticFile(webDir_, req.path, resp)) return true;
        return false;
    }

    // ---- public API routes
    if (req.path == "/api/auth/state" && req.method == "GET") {
        json(resp, 200, "{\"setup_required\":" + b(!auth_->hasPassword()) + ",\"logged_in\":" + b(session) +
                            ",\"user\":" + q(session ? auth_->username() : "") + "}");
        return true;
    }

    const bool stateChanging = req.method != "GET";
    if (stateChanging) {
        auto it = req.headers.find("x-requested-with");
        if (it == req.headers.end() || it->second != "aid-dashboard") {
            error(resp, 403, "missing X-Requested-With header");
            return true;
        }
    }

    if (req.path == "/api/login" && req.method == "POST") {
        std::map<std::string, std::string> kv;
        if (!jsonParseFlat(req.body, &kv) || !kv.count("user") || !kv.count("password")) {
            error(resp, 400, "send {\"user\":...,\"password\":...}");
            return true;
        }
        std::string err;
        int retry = 0;
        const std::string tok = auth_->login(kv["user"], kv["password"], req.clientIp, nowMs, &err, &retry);
        if (tok.empty()) {
            if (retry > 0) {
                resp->headers.push_back({"Retry-After", std::to_string(retry)});
                json(resp, 429, "{\"error\":" + q(err) + ",\"retry_after\":" + std::to_string(retry) + "}");
            } else {
                error(resp, 401, err);
            }
            logWarn("failed dashboard login from " + req.clientIp);
            return true;
        }
        resp->headers.push_back({"Set-Cookie", "aid_session=" + tok + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" +
                                                   std::to_string(maxHours_ * 3600)});
        json(resp, 200, "{\"ok\":true,\"user\":" + q(auth_->username()) + "}");
        logInfo("dashboard login from " + req.clientIp);
        return true;
    }

    // ---- everything else needs a session
    if (!session) { error(resp, 401, "login required"); return true; }
    return handleApi(req, resp, nowMs);
}

bool Api::handleApi(const HttpRequest& req, HttpResponse* resp, int64_t nowMs) {
    const std::string& p = req.path;
    const bool get = req.method == "GET", post = req.method == "POST", del = req.method == "DELETE";
    (void)nowMs;

    if (p == "/api/logout" && post) {
        auth_->logout(cookieValue(req.cookie, "aid_session"));
        resp->headers.push_back({"Set-Cookie", "aid_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0"});
        json(resp, 200, "{\"ok\":true}");
        return true;
    }
    if (p == "/api/status" && get) { json(resp, 200, be_.statusJson ? be_.statusJson() : "{}"); return true; }
    if (p == "/api/snapshot.jpg" && get) {
        std::vector<uint8_t> jpeg;
        std::string e;
        if (be_.snapshot && be_.snapshot(&jpeg, &e)) { resp->contentType = "image/jpeg"; resp->body = jpeg; }
        else error(resp, 503, "snapshot failed: " + e);
        return true;
    }
    if (p == "/api/detections" && get) { json(resp, 200, be_.detectionsJson ? be_.detectionsJson() : "{}"); return true; }
    if (p == "/api/mqtt" && get) { json(resp, 200, be_.mqttJson ? be_.mqttJson() : "{}"); return true; }
    if (p == "/api/system" && get) { json(resp, 200, be_.systemInfoJson ? be_.systemInfoJson() : "{}"); return true; }

    if (p == "/api/logs" && get) {
        long long n = 100;
        parseLong(queryParam(req.query, "n"), &n);
        if (n < 1) n = 1;
        if (n > 300) n = 300;
        std::string o = "{\"lines\":[";
        const std::vector<std::string> lines = recentLogLines((size_t)n);
        for (size_t i = 0; i < lines.size(); ++i) o += (i ? "," : "") + q(lines[i]);
        json(resp, 200, o + "]}");
        return true;
    }

    // ---- events
    if (p == "/api/events" && get) {
        long long limit = 50, since = 0;
        parseLong(queryParam(req.query, "limit"), &limit);
        parseLong(queryParam(req.query, "since"), &since);
        if (limit < 1) limit = 1;
        if (limit > 500) limit = 500;
        std::string type = urlDecode(queryParam(req.query, "type"));
        for (char c : type)
            if (!((c >= 'a' && c <= 'z') || c == '_')) { error(resp, 400, "bad type filter"); return true; }
        const std::vector<Event> ev = events_->query((size_t)limit, type, since);
        std::string o = "{\"total\":" + std::to_string(events_->count()) + ",\"events\":[";
        for (size_t i = 0; i < ev.size(); ++i) o += (i ? "," : "") + EventStore::toJson(ev[i]);
        json(resp, 200, o + "]}");
        return true;
    }
    if (p == "/api/events/export" && get) {
        const std::string fmt = queryParam(req.query, "format");
        const std::vector<Event> ev = events_->query(100000, "", 0);
        std::string o;
        if (fmt == "csv") {
            o = "id,timestamp_ms,type,label,confidence,count,snapshot,detail\n";
            for (auto it = ev.rbegin(); it != ev.rend(); ++it) o += EventStore::toCsvRow(*it) + "\n";
            setText(resp, 200, o, "text/csv; charset=utf-8");
            resp->headers.push_back({"Content-Disposition", "attachment; filename=\"events.csv\""});
        } else if (fmt == "json" || fmt.empty()) {
            o = "[";
            for (auto it = ev.rbegin(); it != ev.rend(); ++it) o += (it == ev.rbegin() ? "" : ",") + EventStore::toJson(*it);
            o += "]";
            setText(resp, 200, o, "application/json");
            resp->headers.push_back({"Content-Disposition", "attachment; filename=\"events.json\""});
        } else {
            error(resp, 400, "format must be csv or json");
        }
        return true;
    }
    if (p == "/api/events" && del) {
        long long before = 0;
        if (queryParam(req.query, "all") == "1") {
            json(resp, 200, "{\"removed\":" + std::to_string(events_->clearAll()) + "}");
        } else if (parseLong(queryParam(req.query, "before"), &before) && before > 0) {
            json(resp, 200, "{\"removed\":" + std::to_string(events_->deleteBefore(before)) + "}");
        } else {
            error(resp, 400, "give before=<ms since epoch> or all=1");
        }
        return true;
    }
    if (p.compare(0, 20, "/api/event_snapshot/") == 0 && get) {
        const std::string path = events_->snapshotPath(p.substr(20));
        if (path.empty()) { error(resp, 400, "bad snapshot name"); return true; }
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) { error(resp, 404, "snapshot not found"); return true; }
        std::vector<uint8_t> data;
        uint8_t buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && data.size() < 4 * 1024 * 1024) data.insert(data.end(), buf, buf + n);
        fclose(f);
        resp->contentType = "image/jpeg";
        resp->body = data;
        return true;
    }

    // ---- config
    if (p == "/api/config" && get) { json(resp, 200, configJson(be_.getConfig())); return true; }
    if (p == "/api/config" && post) {
        std::string err;
        bool restart = false;
        if (!applyConfigPatch(req.body, &err, &restart)) { error(resp, 400, err); return true; }
        json(resp, 200, "{\"ok\":true,\"restart_required\":" + b(restart) + "}");
        logInfo("settings changed from the dashboard");
        return true;
    }

    // ---- password
    if (p == "/api/password" && post) {
        std::map<std::string, std::string> kv;
        if (!jsonParseFlat(req.body, &kv) || !kv.count("old") || !kv.count("new")) { error(resp, 400, "send {\"old\":...,\"new\":...}"); return true; }
        if (!auth_->verifyCurrent(kv["old"])) { error(resp, 403, "the current password is wrong"); return true; }
        std::string err;
        if (!auth_->setPassword(auth_->username(), kv["new"], &err)) { error(resp, 400, err); return true; }
        resp->headers.push_back({"Set-Cookie", "aid_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0"});
        json(resp, 200, "{\"ok\":true}");
        logInfo("dashboard password changed");
        return true;
    }

    // ---- GPIO
    if (p == "/api/gpio" && get) { json(resp, 200, be_.gpioJson ? be_.gpioJson() : "{\"enabled\":false,\"lines\":[]}"); return true; }
    if (p == "/api/gpio" && post) {
        std::map<std::string, std::string> kv;
        if (!jsonParseFlat(req.body, &kv) || !kv.count("name") || !kv.count("active")) { error(resp, 400, "send {\"name\":...,\"active\":true|false}"); return true; }
        std::string err;
        const bool active = kv["active"] == "true" || kv["active"] == "1";
        if (!be_.gpioSet || !be_.gpioSet(kv["name"], active, &err)) { error(resp, 400, err.empty() ? "gpio is not enabled" : err); return true; }
        json(resp, 200, "{\"ok\":true}");
        return true;
    }

    // ---- system actions (fixed list, explicit confirmation)
    if (p == "/api/system" && post) {
        std::map<std::string, std::string> kv;
        if (!jsonParseFlat(req.body, &kv) || !kv.count("action")) { error(resp, 400, "send {\"action\":...,\"confirm\":\"yes\"}"); return true; }
        const std::string& a = kv["action"];
        if (a != "restart_service" && a != "reboot" && a != "shutdown") { error(resp, 400, "unknown action"); return true; }
        if (kv["confirm"] != "yes") { error(resp, 400, "confirmation required"); return true; }
        if (!be_.systemAction || !be_.systemAction(a)) { error(resp, 500, "action failed"); return true; }
        json(resp, 200, "{\"ok\":true}");
        logWarn("system action from the dashboard: " + a);
        return true;
    }

    error(resp, 404, "unknown endpoint");
    return true;
}

}  // namespace aid
