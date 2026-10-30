// PC-side tests for the dashboard API: login/session/CSRF rules, protected routes, settings
// validation, events, export, delete, snapshot-name safety, password change, system actions.
// Uses a real AuthManager and EventStore (temp directory) and fake backends; no sockets.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

#include "api.h"
#include "json_util.h"
#include "log.h"

static int g_fail = 0, g_run = 0;
#define CHECK(cond)                                                   \
    do {                                                              \
        ++g_run;                                                      \
        if (!(cond)) {                                                \
            ++g_fail;                                                 \
            printf("FAIL line %d: %s\n", __LINE__, #cond);            \
        }                                                             \
    } while (0)

static std::string body(const aid::HttpResponse& r) { return std::string(r.body.begin(), r.body.end()); }
static bool has(const std::string& s, const char* p) { return s.find(p) != std::string::npos; }
static std::string setCookie(const aid::HttpResponse& r) {
    for (const auto& h : r.headers) if (h.first == "Set-Cookie") return h.second;
    return "";
}

static aid::HttpRequest req(const std::string& method, const std::string& path, const std::string& query = "",
                            const std::string& bodyText = "", const std::string& cookie = "", bool csrf = true) {
    aid::HttpRequest r;
    r.method = method; r.path = path; r.query = query; r.body = bodyText; r.cookie = cookie;
    r.clientIp = "10.0.0.5";
    if (csrf) r.headers["x-requested-with"] = "aid-dashboard";
    return r;
}

int main() {
    char tmpl[] = "/tmp/aidapiXXXXXX";
    if (!mkdtemp(tmpl)) return 2;
    const std::string dir = tmpl;
    mkdir((dir + "/web").c_str(), 0700);
    { std::ofstream f((dir + "/web/index.html").c_str()); f << "<html>dash</html>"; }

    std::string err;
    aid::AuthManager auth(dir + "/auth.json", 15, 8);
    CHECK(auth.setPassword("admin", "dash-pass-1", &err));
    aid::EventStore events(dir + "/data", 100, 5);
    CHECK(events.init(&err));
    for (int i = 1; i <= 3; ++i) {
        aid::Event e;
        e.id = (uint64_t)i; e.tsMs = 1000 * i; e.type = i == 2 ? "motion_detected" : "person_detected";
        e.label = "person"; e.confidence = 0.8f;
        e.snapshot = events.saveSnapshot(e.id, std::vector<uint8_t>(20, (uint8_t)i));
        events.add(e);
    }

    aid::Config cfg;
    int applyCalls = 0;
    std::string lastAction;
    aid::ApiBackend be;
    be.statusJson = [] { return std::string("{\"version\":\"test\"}"); };
    be.snapshot = [](std::vector<uint8_t>* j, std::string*) { j->assign(5, 0xAB); return true; };
    be.getConfig = [&] { return cfg; };
    be.applyConfig = [&](const aid::Config& c, std::string*, bool*) { cfg = c; ++applyCalls; return true; };
    be.systemAction = [&](const std::string& a) { lastAction = a; return true; };
    be.gpioJson = [] { return std::string("{\"enabled\":false,\"lines\":[]}"); };
    be.gpioSet = [](const std::string& n, bool, std::string* e) { if (n != "relay1") { *e = "unknown gpio"; return false; } return true; };

    const std::string token = "machine-token-0123456789";
    aid::Api api(&auth, &events, dir + "/web", token, 8, be);
    aid::HttpResponse r;
    int64_t t = 5000000;
    api.setWallClock([] { return (int64_t)1700000000123LL; });   // photos are named by wall-clock time

    // public routes
    CHECK(api.handle(req("GET", "/health"), &r, t) && body(r) == "ok\n");
    r = aid::HttpResponse();
    CHECK(api.handle(req("GET", "/"), &r, t) && has(body(r), "dash"));
    r = aid::HttpResponse();
    CHECK(!api.handle(req("GET", "/missing.html"), &r, t));                       // -> 404 by the server
    r = aid::HttpResponse();
    CHECK(api.handle(req("GET", "/api/auth/state"), &r, t) && has(body(r), "\"logged_in\":false") && has(body(r), "\"setup_required\":false"));

    // everything protected refuses anonymous callers
    const char* protectedGet[] = {"/api/status", "/api/events", "/api/config", "/api/logs", "/api/detections",
                                  "/api/gpio", "/api/system", "/api/snapshot.jpg", "/api/events/export",
                                  "/api/event_snapshot/1.jpg", "/api/mqtt"};
    for (const char* p : protectedGet) {
        r = aid::HttpResponse();
        api.handle(req("GET", p), &r, t);
        CHECK(r.status == 401);
    }
    r = aid::HttpResponse();
    api.handle(req("GET", "/snapshot.jpg"), &r, t);
    CHECK(r.status == 401);
    r = aid::HttpResponse();
    api.handle(req("GET", "/status.json"), &r, t);
    CHECK(r.status == 401);

    // machine token: only the two read-only endpoints
    aid::HttpRequest tr = req("GET", "/snapshot.jpg", "token=" + token);
    r = aid::HttpResponse();
    api.handle(tr, &r, t);
    CHECK(r.status == 200 && r.contentType == "image/jpeg" && r.body.size() == 5);
    tr = req("GET", "/status.json");
    tr.bearer = token;
    r = aid::HttpResponse();
    api.handle(tr, &r, t);
    CHECK(r.status == 200 && has(body(r), "test"));
    tr = req("GET", "/api/config");
    tr.bearer = token;                                           // the token does NOT open the admin API
    r = aid::HttpResponse();
    api.handle(tr, &r, t);
    CHECK(r.status == 401);

    // the token also reads events + their pictures and can store a photo, nothing more
    tr = req("GET", "/api/events", "after_id=1");
    tr.bearer = token;
    r = aid::HttpResponse();
    api.handle(tr, &r, t);
    CHECK(r.status == 200 && has(body(r), "\"id\":3") && has(body(r), "\"id\":2") && !has(body(r), "\"id\":1,") &&
          has(body(r), "\"last_id\":3"));
    tr = req("GET", "/api/event_snapshot/2.jpg");
    tr.bearer = token;
    r = aid::HttpResponse();
    api.handle(tr, &r, t);
    CHECK(r.status == 200 && r.body.size() == 20);
    tr = req("POST", "/api/capture");
    tr.bearer = token;
    r = aid::HttpResponse();
    api.handle(tr, &r, t);
    CHECK(r.status == 200 && has(body(r), "1700000000123.jpg"));
    tr = req("POST", "/api/capture", "", "", "", false);          // no CSRF header
    tr.bearer = token;
    r = aid::HttpResponse();
    api.handle(tr, &r, t);
    CHECK(r.status == 403);
    tr = req("POST", "/api/capture");
    tr.bearer = "wrong-token";
    r = aid::HttpResponse();
    api.handle(tr, &r, t);
    CHECK(r.status == 401);
    tr = req("GET", "/api/captures");
    tr.bearer = token;
    r = aid::HttpResponse();
    api.handle(tr, &r, t);
    CHECK(r.status == 200 && has(body(r), "1700000000123.jpg"));
    tr = req("GET", "/api/capture/1700000000123.jpg");
    tr.bearer = token;
    r = aid::HttpResponse();
    api.handle(tr, &r, t);
    CHECK(r.status == 200 && r.contentType == "image/jpeg" && r.body.size() == 5);
    tr = req("GET", "/api/capture/..%2f1700000000123.jpg");
    tr.bearer = token;
    r = aid::HttpResponse();
    api.handle(tr, &r, t);
    CHECK(r.status == 400 || r.status == 404);
    tr = req("DELETE", "/api/events", "all=1");
    tr.bearer = token;                                           // the token cannot delete
    r = aid::HttpResponse();
    api.handle(tr, &r, t);
    CHECK(r.status == 401 && events.count() == 3);
    r = aid::HttpResponse();
    api.handle(req("GET", "/api/captures"), &r, t);               // anonymous: no
    CHECK(r.status == 401);

    // login: CSRF header, wrong password, then success
    r = aid::HttpResponse();
    api.handle(req("POST", "/api/login", "", "{\"user\":\"admin\",\"password\":\"dash-pass-1\"}", "", false), &r, t);
    CHECK(r.status == 403);
    r = aid::HttpResponse();
    api.handle(req("POST", "/api/login", "", "{\"user\":\"admin\",\"password\":\"nope-nope-1\"}"), &r, t);
    CHECK(r.status == 401 && setCookie(r).empty());
    r = aid::HttpResponse();
    api.handle(req("POST", "/api/login", "", "garbage"), &r, t);
    CHECK(r.status == 400);
    r = aid::HttpResponse();
    api.handle(req("POST", "/api/login", "", "{\"user\":\"admin\",\"password\":\"dash-pass-1\"}"), &r, t);
    CHECK(r.status == 200);
    const std::string sc = setCookie(r);
    CHECK(has(sc, "HttpOnly") && has(sc, "SameSite=Strict") && has(sc, "Path=/") && has(sc, "Max-Age=28800"));
    const std::string cookie = sc.substr(0, sc.find(';'));          // "aid_session=<64 hex>"
    CHECK(cookie.size() == 12 + 64);

    // with the session
    r = aid::HttpResponse();
    api.handle(req("GET", "/api/auth/state", "", "", cookie), &r, t + 1000);
    CHECK(has(body(r), "\"logged_in\":true") && has(body(r), "admin"));
    r = aid::HttpResponse();
    api.handle(req("GET", "/api/status", "", "", cookie), &r, t + 1000);
    CHECK(r.status == 200);
    r = aid::HttpResponse();
    api.handle(req("GET", "/api/snapshot.jpg", "", "", cookie), &r, t + 1000);
    CHECK(r.status == 200 && r.body.size() == 5);

    // events: list, filter, limit, export, snapshots, delete
    r = aid::HttpResponse();
    api.handle(req("GET", "/api/events", "limit=2", "", cookie), &r, t + 2000);
    CHECK(has(body(r), "\"total\":3") && has(body(r), "\"id\":3") && !has(body(r), "\"id\":1,"));
    r = aid::HttpResponse();
    api.handle(req("GET", "/api/events", "type=motion_detected", "", cookie), &r, t + 2000);
    CHECK(has(body(r), "\"id\":2") && !has(body(r), "\"id\":3"));
    r = aid::HttpResponse();
    api.handle(req("GET", "/api/events", "type=../etc", "", cookie), &r, t + 2000);
    CHECK(r.status == 400);
    r = aid::HttpResponse();
    api.handle(req("GET", "/api/events/export", "format=csv", "", cookie), &r, t + 2000);
    bool attachment = false;
    for (const auto& h : r.headers) if (h.first == "Content-Disposition" && has(h.second, "attachment")) attachment = true;
    CHECK(r.status == 200 && r.contentType.find("text/csv") == 0 && has(body(r), "id,timestamp_ms,type") && attachment);
    r = aid::HttpResponse();
    api.handle(req("GET", "/api/events/export", "format=xml", "", cookie), &r, t + 2000);
    CHECK(r.status == 400);
    r = aid::HttpResponse();
    api.handle(req("GET", "/api/event_snapshot/2.jpg", "", "", cookie), &r, t + 2000);
    CHECK(r.status == 200 && r.contentType == "image/jpeg" && r.body.size() == 20);
    const char* badNames[] = {"../auth.json", "2.jpg/../../x", "x.jpg", "2.png", "..%2f2.jpg", ""};
    for (const char* n : badNames) {
        r = aid::HttpResponse();
        api.handle(req("GET", std::string("/api/event_snapshot/") + n, "", "", cookie), &r, t + 2000);
        CHECK(r.status == 400 || r.status == 404);
    }
    r = aid::HttpResponse();
    api.handle(req("DELETE", "/api/events", "before=2500", "", cookie), &r, t + 2000);
    CHECK(has(body(r), "\"removed\":2"));
    r = aid::HttpResponse();
    api.handle(req("DELETE", "/api/events", "", "", cookie), &r, t + 2000);
    CHECK(r.status == 400);                                                    // refuses a bare delete
    r = aid::HttpResponse();
    api.handle(req("DELETE", "/api/events", "all=1", "", cookie, false), &r, t + 2000);
    CHECK(r.status == 403);                                                    // CSRF header missing
    r = aid::HttpResponse();
    api.handle(req("DELETE", "/api/events", "all=1", "", cookie), &r, t + 2000);
    CHECK(has(body(r), "\"removed\":1") && events.count() == 0);

    // settings: read (secrets masked), valid change, rejected changes, secrets kept
    cfg.httpToken = "keep-this-token-1234";
    cfg.mqttPassword = "mqtt-secret";
    r = aid::HttpResponse();
    api.handle(req("GET", "/api/config", "", "", cookie), &r, t + 3000);
    CHECK(r.status == 200 && has(body(r), "\"http_token_set\":true") && has(body(r), "\"mqtt_password_set\":true") &&
          !has(body(r), "keep-this-token") && !has(body(r), "mqtt-secret"));
    r = aid::HttpResponse();
    api.handle(req("POST", "/api/config", "", "{\"detect_conf_pct\":60,\"detect_enabled\":false,\"zone\":\"0.1,0.1,0.5,0.5;0.6,0.6,0.9,0.9\","
                                              "\"detect_classes\":\"person,car\",\"http_token\":\"\"}", cookie), &r, t + 3000);
    CHECK(r.status == 200 && has(body(r), "\"restart_required\":false"));
    CHECK(cfg.detectConfPct == 60 && !cfg.detectEnabled && cfg.zones.size() == 2 && cfg.detectClasses.size() == 2);
    CHECK(cfg.httpToken == "keep-this-token-1234");                           // empty secret = unchanged
    r = aid::HttpResponse();
    api.handle(req("POST", "/api/config", "", "{\"width\":1280,\"height\":720}", cookie), &r, t + 3000);
    CHECK(r.status == 200 && has(body(r), "\"restart_required\":true"));
    r = aid::HttpResponse();
    api.handle(req("POST", "/api/config", "", "{\"zone\":\"\"}", cookie), &r, t + 3000);
    CHECK(r.status == 200 && cfg.zones.empty());
    const int before = applyCalls;
    const char* badBodies[] = {"{\"detect_conf_pct\":500}", "{\"data_dir\":\"/etc\"}", "{\"auth_file\":\"/tmp/x\"}",
                               "{\"nonsense\":1}", "{\"zone\":\"0.9,0.9,0.1,0.1\"}", "{\"mqtt_prefix\":\"a/b/\"}",
                               "{\"detect_classes\":\"x\\ny\"}", "{\"fps\":\"fast\"}", "not json", "{}",
                               "{\"detect_conf_pct\":{\"a\":1}}", "{\"gpio\":\"x,toaster,5,0\"}", "{\"rtsp_path\":\"live\"}"};
    for (const char* bb : badBodies) {
        r = aid::HttpResponse();
        api.handle(req("POST", "/api/config", "", bb, cookie), &r, t + 3000);
        CHECK(r.status == 400);
    }
    CHECK(applyCalls == before);                                              // nothing invalid reached the backend
    CHECK(cfg.detectConfPct == 60);

    // system actions need the confirmation and a known name
    r = aid::HttpResponse();
    api.handle(req("POST", "/api/system", "", "{\"action\":\"reboot\"}", cookie), &r, t + 4000);
    CHECK(r.status == 400 && lastAction.empty());
    r = aid::HttpResponse();
    api.handle(req("POST", "/api/system", "", "{\"action\":\"rm -rf /\",\"confirm\":\"yes\"}", cookie), &r, t + 4000);
    CHECK(r.status == 400 && lastAction.empty());
    r = aid::HttpResponse();
    api.handle(req("POST", "/api/system", "", "{\"action\":\"restart_service\",\"confirm\":\"yes\"}", cookie), &r, t + 4000);
    CHECK(r.status == 200 && lastAction == "restart_service");

    // gpio
    r = aid::HttpResponse();
    api.handle(req("POST", "/api/gpio", "", "{\"name\":\"relay1\",\"active\":true}", cookie), &r, t + 4000);
    CHECK(r.status == 200);
    r = aid::HttpResponse();
    api.handle(req("POST", "/api/gpio", "", "{\"name\":\"zzz\",\"active\":true}", cookie), &r, t + 4000);
    CHECK(r.status == 400);

    // logs
    aid::logInfo("api test line");
    r = aid::HttpResponse();
    api.handle(req("GET", "/api/logs", "n=5", "", cookie), &r, t + 4000);
    CHECK(r.status == 200 && has(body(r), "api test line"));

    // session timeouts: 20 minutes of silence ends the session
    r = aid::HttpResponse();
    api.handle(req("GET", "/api/status", "", "", cookie), &r, t + 4000 + 20 * 60000);
    CHECK(r.status == 401);

    // password change ends sessions; wrong old password refused
    r = aid::HttpResponse();
    api.handle(req("POST", "/api/login", "", "{\"user\":\"admin\",\"password\":\"dash-pass-1\"}"), &r, t + 5000000);
    const std::string sc2 = setCookie(r);
    const std::string cookie2 = sc2.substr(0, sc2.find(';'));
    r = aid::HttpResponse();
    api.handle(req("POST", "/api/password", "", "{\"old\":\"wrong-old-pw\",\"new\":\"brand-new-pass\"}", cookie2), &r, t + 5000100);
    CHECK(r.status == 403);
    r = aid::HttpResponse();
    api.handle(req("POST", "/api/password", "", "{\"old\":\"dash-pass-1\",\"new\":\"short\"}", cookie2), &r, t + 5000100);
    CHECK(r.status == 400);
    r = aid::HttpResponse();
    api.handle(req("POST", "/api/password", "", "{\"old\":\"dash-pass-1\",\"new\":\"brand-new-pass\"}", cookie2), &r, t + 5000100);
    CHECK(r.status == 200 && has(setCookie(r), "Max-Age=0"));
    r = aid::HttpResponse();
    api.handle(req("GET", "/api/status", "", "", cookie2), &r, t + 5000200);
    CHECK(r.status == 401);

    // logout
    r = aid::HttpResponse();
    api.handle(req("POST", "/api/login", "", "{\"user\":\"admin\",\"password\":\"brand-new-pass\"}"), &r, t + 5100000);
    const std::string sc3 = setCookie(r);
    const std::string cookie3 = sc3.substr(0, sc3.find(';'));
    r = aid::HttpResponse();
    api.handle(req("POST", "/api/logout", "", "", cookie3), &r, t + 5100100);
    CHECK(r.status == 200);
    r = aid::HttpResponse();
    api.handle(req("GET", "/api/status", "", "", cookie3), &r, t + 5100200);
    CHECK(r.status == 401);

    // brute force: 5 wrong passwords lock the client out
    aid::HttpRequest bad = req("POST", "/api/login", "", "{\"user\":\"admin\",\"password\":\"guess-guess\"}");
    bad.clientIp = "6.6.6.6";
    for (int i = 0; i < 5; ++i) { r = aid::HttpResponse(); api.handle(bad, &r, t + 6000000 + i); }
    r = aid::HttpResponse();
    api.handle(bad, &r, t + 6000100);
    CHECK(r.status == 429);
    aid::HttpRequest good = req("POST", "/api/login", "", "{\"user\":\"admin\",\"password\":\"brand-new-pass\"}");
    good.clientIp = "6.6.6.6";
    r = aid::HttpResponse();
    api.handle(good, &r, t + 6000200);
    CHECK(r.status == 429);                                                    // even the right one is refused while locked

    // jsonParseFlat edge cases
    std::map<std::string, std::string> kv;
    CHECK(aid::jsonParseFlat("{\"a\":1,\"b\":\"x y\",\"c\":true,\"d\":-2.5}", &kv) && kv.size() == 4 && kv["d"] == "-2.5" && kv["b"] == "x y");
    CHECK(aid::jsonParseFlat("{}", &kv) && kv.empty());
    CHECK(aid::jsonParseFlat("  { \"a\" : \"q\\\"r\" }  ", &kv) && kv["a"] == "q\"r");
    CHECK(!aid::jsonParseFlat("{\"a\":[1]}", &kv));
    CHECK(!aid::jsonParseFlat("{\"a\":1", &kv));
    CHECK(!aid::jsonParseFlat("{\"a\":1}x", &kv));
    CHECK(!aid::jsonParseFlat("{a:1}", &kv));
    CHECK(!aid::jsonParseFlat("{\"a\":\"\\q\"}", &kv));
    CHECK(!aid::jsonParseFlat(std::string(20000, ' ') + "{}", &kv));

    std::string cmd = std::string("rm -rf ") + tmpl;
    CHECK(system(cmd.c_str()) == 0);
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
