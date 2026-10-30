// PC-side unit tests: config, JSON helpers, event engine, event store. Run: bash scripts/test.sh
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "config.h"
#include "event_engine.h"
#include "event_store.h"
#include "json_util.h"

static int g_fail = 0, g_run = 0;
#define CHECK(cond)                                                   \
    do {                                                              \
        ++g_run;                                                      \
        if (!(cond)) {                                                \
            ++g_fail;                                                 \
            printf("FAIL line %d: %s\n", __LINE__, #cond);            \
        }                                                             \
    } while (0)

static aid::Detection det(const char* label, float conf, float cx, float cy) {
    aid::Detection d;
    d.label = label;
    d.conf = conf;
    d.x1 = cx - 0.05f; d.x2 = cx + 0.05f; d.y1 = cy - 0.05f; d.y2 = cy + 0.05f;
    return d;
}

static void testConfig() {
    aid::Config c;
    std::vector<std::string> p;
    CHECK(aid::parseConfig("zone=0.1,0.1,0.5,0.5\nzone=0.6,0.6,0.9,0.9\ndetect_classes=person, car\n"
                           "gpio=relay1,relay,41,0\ngpio=pir,pir,42,1\nmqtt_prefix=cam1\n", &c, &p));
    CHECK(p.empty());
    CHECK(c.zones.size() == 2 && c.zones[1].x1 > 0.59f && c.zones[1].x1 < 0.61f);
    CHECK(c.detectClasses.size() == 2 && c.detectClasses[1] == "car");
    CHECK(c.gpio.size() == 2 && c.gpio[0].output && !c.gpio[1].output && c.gpio[1].activeLow);
    CHECK(c.mqttPrefix == "cam1");

    // invalid values are rejected and not applied
    aid::Config d;
    p.clear();
    CHECK(!aid::parseConfig("zone=0.5,0.5,0.1,0.1\n", &d, &p));
    CHECK(d.zones.empty());
    p.clear();
    CHECK(!aid::parseConfig("gpio=x,toaster,5,0\n", &d, &p));
    p.clear();
    CHECK(!aid::parseConfig("data_dir=/root/../etc\n", &d, &p));
    p.clear();
    CHECK(!aid::parseConfig("mqtt_prefix=a/#\n", &d, &p));    // '#' starts a comment, leaving "a/"
    p.clear();
    CHECK(!aid::parseConfig("mqtt_prefix=a+b\n", &d, &p));
    p.clear();
    CHECK(!aid::parseConfig("detect_interval_ms=5\n", &d, &p));
    p.clear();
    CHECK(!aid::parseConfig("session_idle_min=0\n", &d, &p));

    // serialize -> parse round trip
    aid::Config e;
    e.width = 1280; e.height = 720; e.detectConfPct = 60; e.httpToken = "0123456789abcdef";
    e.mqttEnabled = true; e.mqttHost = "192.168.1.5"; e.mqttPassword = "pa ss";
    e.zones.push_back({0.2f, 0.2f, 0.8f, 0.8f});
    aid::Config f;
    p.clear();
    CHECK(aid::parseConfig(aid::serializeConfig(e), &f, &p));
    CHECK(p.empty());
    CHECK(f.width == 1280 && f.detectConfPct == 60 && f.httpToken == e.httpToken);
    CHECK(f.mqttEnabled && f.mqttHost == "192.168.1.5" && f.mqttPassword == "pa ss");
    CHECK(f.zones.size() == 1);

    // save to a file with mode 0600 and load back
    char tmpl[] = "/tmp/aidcfgXXXXXX";
    const int fd = mkstemp(tmpl);
    CHECK(fd >= 0);
    close(fd);
    std::string err;
    CHECK(aid::saveConfigFile(tmpl, e, &err));
    struct stat st;
    CHECK(stat(tmpl, &st) == 0 && (st.st_mode & 0777) == 0600);
    aid::Config g;
    p.clear();
    CHECK(aid::loadConfigFile(tmpl, &g, &p, &err) && g.height == 720);
    unlink(tmpl);
}

static void testJson() {
    std::string v;
    CHECK(aid::jsonGetFlat("{\"a\":\"x y\",\"b\":5,\"c\":true}", "a", &v) && v == "x y");
    CHECK(aid::jsonGetFlat("{\"a\":\"x y\",\"b\":5,\"c\":true}", "b", &v) && v == "5");
    CHECK(aid::jsonGetFlat("{\"a\":\"x y\",\"b\":5,\"c\":true}", "c", &v) && v == "true");
    CHECK(!aid::jsonGetFlat("{\"a\":1}", "zzz", &v));
    CHECK(aid::jsonGetFlat("{\"q\":\"say \\\"hi\\\"\\n\"}", "q", &v) && v == "say \"hi\"\n");
    CHECK(!aid::jsonGetFlat("{\"q\":\"unterminated", "q", &v));
    CHECK(aid::jsonEscape("a\"b\\c\n\x01") == "a\\\"b\\\\c\\n\\u0001");
}

struct Collector {
    std::vector<aid::Event> ev;
    int count(const std::string& type) const {
        int n = 0;
        for (const auto& e : ev) if (e.type == type) ++n;
        return n;
    }
};

static void testEngine() {
    aid::Config cfg;
    cfg.eventEnterFrames = 2;
    cfg.eventCooldownS = 30;
    cfg.eventAbsentS = 10;
    cfg.detectConfPct = 50;
    aid::EventEngine eng(cfg);
    Collector col;
    eng.setSink([&](aid::Event& e) { col.ev.push_back(e); });

    int64_t t = 1000;
    const int64_t wall = 1700000000000LL;
    std::vector<aid::Detection> person = {det("person", 0.9f, 0.5f, 0.5f)};
    std::vector<aid::Detection> none;

    eng.onDetections(person, t, wall);                 // first sighting: not confirmed yet
    CHECK(col.ev.empty());
    t += 500;
    eng.onDetections(person, t, wall + 500);           // second: confirmed
    CHECK(col.count("person_detected") == 1);
    CHECK(col.count("object_entered") == 1);
    CHECK(eng.groupPresent("person"));
    CHECK(eng.currentCounts().at("person") == 1);
    CHECK(col.ev[0].id == 1 && col.ev[1].id == 2);

    for (int i = 0; i < 5; ++i) { t += 500; eng.onDetections(person, t, wall + t); }
    CHECK(col.ev.size() == 2);                         // no duplicates while it stays in view

    t += 5000;
    eng.onDetections(none, t, wall + t);               // gone for 5 s: not yet "disappeared"
    CHECK(col.count("object_disappeared") == 0);
    t += 6000;
    eng.onDetections(none, t, wall + t);               // gone > 10 s
    CHECK(col.count("object_disappeared") == 1);
    CHECK(!eng.groupPresent("person"));

    // comes back within the cooldown: no new person_detected
    t += 1000; eng.onDetections(person, t, wall + t);
    t += 500;  eng.onDetections(person, t, wall + t);
    CHECK(col.count("person_detected") == 1);
    // after the cooldown a new one is raised
    eng.onDetections(none, t + 20000, wall + t);       // disappears again
    t += 40000; eng.onDetections(person, t, wall + t);
    t += 500;   eng.onDetections(person, t, wall + t);
    CHECK(col.count("person_detected") == 2);

    // filters: low confidence, wrong class, outside zone
    aid::Config cfg2 = cfg;
    cfg2.zones.push_back({0.0f, 0.0f, 0.5f, 0.5f});
    aid::EventEngine eng2(cfg2);
    Collector col2;
    eng2.setSink([&](aid::Event& e) { col2.ev.push_back(e); });
    int64_t u = 0;
    for (int i = 0; i < 3; ++i) {
        u += 500;
        eng2.onDetections({det("person", 0.3f, 0.2f, 0.2f)}, u, u);   // below 50 %
        eng2.onDetections({det("chair", 0.9f, 0.2f, 0.2f)}, u, u);    // not an enabled class
        eng2.onDetections({det("person", 0.9f, 0.8f, 0.8f)}, u, u);   // outside the zone
    }
    CHECK(col2.ev.empty());
    for (int i = 0; i < 2; ++i) { u += 500; eng2.onDetections({det("car", 0.8f, 0.2f, 0.2f)}, u, u); }
    CHECK(col2.count("vehicle_detected") == 1);

    // motion, camera and system state
    aid::EventEngine eng3(cfg);
    Collector col3;
    eng3.setSink([&](aid::Event& e) { col3.ev.push_back(e); });
    eng3.onMotion(true, 0, 1);
    eng3.onMotion(true, 100, 2);                       // still moving: no duplicate
    CHECK(col3.count("motion_detected") == 1);
    eng3.onMotion(false, 200, 3);
    eng3.onMotion(true, 5000, 4);                      // inside cooldown: suppressed
    CHECK(col3.count("motion_detected") == 1);
    eng3.onMotion(false, 5100, 5);
    eng3.onMotion(true, 40000, 6);                     // after cooldown
    CHECK(col3.count("motion_detected") == 2);
    eng3.onCameraState(true, 0, 1);                    // no change from the initial "ok"
    CHECK(col3.count("camera_disconnected") == 0);
    eng3.onCameraState(false, 10, 2);
    eng3.onCameraState(false, 20, 3);
    CHECK(col3.count("camera_disconnected") == 1);
    eng3.onCameraState(true, 30, 4);
    CHECK(col3.count("camera_reconnected") == 1);
    eng3.onSystemState(true, 0, 1);
    eng3.onSystemState(true, 1, 2);
    CHECK(col3.count("system_online") == 1);
    eng3.onSystemState(false, 2, 3);
    CHECK(col3.count("system_offline") == 1);

    CHECK(aid::eventGroupOf("truck") == "vehicle");
    CHECK(aid::eventGroupOf("cat") == "animal");
    CHECK(aid::eventGroupOf("bicycle") == "bicycle");
}

static void testStore() {
    char tmpl[] = "/tmp/aidstoreXXXXXX";
    CHECK(mkdtemp(tmpl) != nullptr);
    const std::string dir = std::string(tmpl) + "/data";   // parent does not exist yet: init must create it
    std::string err;
    {
        aid::EventStore st(dir, 5, 3);
        CHECK(st.init(&err));
        for (int i = 1; i <= 8; ++i) {
            aid::Event e;
            e.id = (uint64_t)i;
            e.tsMs = 1000 * i;
            e.type = (i % 2) ? "person_detected" : "motion_detected";
            e.label = "say \"hi\"";
            e.confidence = 0.5f;
            e.snapshot = st.saveSnapshot(e.id, std::vector<uint8_t>(10, (uint8_t)i));
            st.add(e);
        }
        CHECK(st.count() == 5);                             // bounded
        std::vector<aid::Event> q = st.query(100, "", 0);
        CHECK(q.size() == 5 && q[0].id == 8 && q[4].id == 4);  // newest first
        CHECK(st.query(100, "motion_detected", 0).size() == 3);   // ids 4, 6, 8
        CHECK(st.query(100, "person_detected", 0).size() == 2);   // ids 5, 7
        CHECK(st.query(100, "", 6000).size() == 3);
        CHECK(st.query(2, "", 0).size() == 2);
        CHECK(q[0].label == "say \"hi\"");

        // only the 3 newest snapshot files survive, and events that lost theirs were trimmed
        CHECK(access((dir + "/snapshots/8.jpg").c_str(), F_OK) == 0);
        CHECK(access((dir + "/snapshots/6.jpg").c_str(), F_OK) == 0);
        CHECK(access((dir + "/snapshots/5.jpg").c_str(), F_OK) != 0);

        CHECK(st.snapshotPath("8.jpg") == dir + "/snapshots/8.jpg");
        CHECK(st.snapshotPath("../x.jpg").empty());
        CHECK(st.snapshotPath("8.jpg/../../etc").empty());
        CHECK(st.snapshotPath("abc.jpg").empty());
        CHECK(st.snapshotPath("8.png").empty());
    }
    {   // reload from disk
        aid::EventStore st(dir, 5, 3);
        CHECK(st.init(&err));
        CHECK(st.count() >= 1 && st.count() <= 8);
        std::vector<aid::Event> q = st.query(100, "", 0);
        CHECK(!q.empty() && q[0].id == 8 && q[0].label == "say \"hi\"");
        CHECK(st.deleteBefore(7000) >= 1);
        CHECK(st.query(100, "", 0).size() == 2);            // ids 7 and 8 remain
        CHECK(st.clearAll() == 2);
        CHECK(st.count() == 0);
    }
    {
        aid::Event e;
        e.id = 5; e.tsMs = 42; e.type = "x"; e.label = "a,\"b\"";
        CHECK(aid::EventStore::toCsvRow(e).find("\"a,\"\"b\"\"\"") != std::string::npos);
        aid::Event r;
        CHECK(aid::EventStore::fromJson(aid::EventStore::toJson(e), &r) && r.id == 5 && r.label == "a,\"b\"");
        CHECK(!aid::EventStore::fromJson("{\"id\":0,\"ts\":1,\"type\":\"x\"}", &r));
        CHECK(!aid::EventStore::fromJson("not json", &r));
    }
    std::string cmd = std::string("rm -rf ") + tmpl;
    CHECK(system(cmd.c_str()) == 0);
}

int main() {
    testConfig();
    testJson();
    testEngine();
    testStore();
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
