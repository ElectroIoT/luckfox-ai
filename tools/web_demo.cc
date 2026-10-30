// PC-only demo server for the dashboard: serves web/ with fake camera data on 127.0.0.1 so the
// pages can be tried in a browser without the board. NOT part of the board build.
//   web_demo <web dir> <jpeg file> [port]       login: admin / demo-pass-123 (a test value)
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "api.h"
#include "json_util.h"
#include "log.h"

static std::atomic<bool> g_quit{false};
static void onSig(int) { g_quit = true; }

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <web dir> <jpeg> [port]\n", argv[0]); return 2; }
    const std::string webDir = argv[1];
    const int port = argc > 3 ? atoi(argv[3]) : 18090;
    signal(SIGINT, onSig);
    signal(SIGTERM, onSig);

    std::ifstream jf(argv[2], std::ios::binary);
    std::ostringstream jb;
    jb << jf.rdbuf();
    const std::string jpegStr = jb.str();
    const std::vector<uint8_t> jpeg(jpegStr.begin(), jpegStr.end());

    char tmpl[] = "/tmp/aiddemoXXXXXX";
    if (!mkdtemp(tmpl)) return 2;
    const std::string dir = tmpl;
    std::string err;
    aid::AuthManager auth(dir + "/auth.json", 15, 8);
    if (!auth.setPassword("admin", "demo-pass-123", &err)) { fprintf(stderr, "%s\n", err.c_str()); return 2; }
    aid::EventStore events(dir + "/data", 200, 20);
    events.init(&err);
    const char* types[] = {"person_detected", "object_entered", "motion_detected", "vehicle_detected", "object_disappeared", "camera_disconnected", "system_online"};
    const char* labels[] = {"person", "person", "", "car", "person", "camera", "system"};
    const int64_t now = (int64_t)time(nullptr) * 1000;
    for (int i = 0; i < 14; ++i) {
        aid::Event e;
        e.id = (uint64_t)i + 1;
        e.tsMs = now - (int64_t)(14 - i) * 3600 * 1000;
        e.type = types[i % 7];
        e.label = labels[i % 7];
        e.confidence = (i % 7 < 4) ? 0.55f + 0.03f * (float)(i % 9) : 0.0f;
        e.count = 1;
        if (i % 7 < 4) e.snapshot = events.saveSnapshot(e.id, jpeg);
        events.add(e);
    }

    aid::Config cfg;
    cfg.httpToken = "demo-machine-token-0123456789";
    cfg.zones.push_back(aid::ZoneRect(0.2f, 0.25f, 0.8f, 0.9f));
    std::atomic<int> tick{0};

    aid::ApiBackend be;
    be.statusJson = [&]() -> std::string {
        const int t = tick++;
        char buf[1800];
        snprintf(buf, sizeof(buf),
                 "{\"version\":\"demo\",\"uptime_s\":%d,\"time_ms\":%lld,"
                 "\"camera\":{\"sensor\":\"m00_b_sc3336 4-0030\",\"ok\":true,\"width\":1920,\"height\":1080,\"fps_config\":30},"
                 "\"stream\":{\"running\":true,\"rtsp_port\":554,\"rtsp_path\":\"/live/0\",\"bitrate_kbps_config\":2048,\"frames\":%d,\"keyframes\":%d,"
                 "\"recent_fps\":29.9,\"recent_kbps\":%.1f,\"encode_latency_avg_ms\":44.6,\"encode_latency_max_ms\":45.0,\"snapshots\":%d,"
                 "\"last_snapshot_ms\":{\"frame_wait\":7.5,\"encode\":12.1,\"total\":19.6}},"
                 "\"ai\":{\"enabled\":true,\"model_loaded\":true,\"runs\":%d,\"interval_ms\":500,\"conf_pct\":45,\"last_pre_ms\":14.0,\"last_infer_ms\":92.0,"
                 "\"last_post_ms\":6.0,\"avg_infer_ms\":91.5,\"last_count\":1,\"error\":\"\"},"
                 "\"motion\":{\"enabled\":true,\"active\":%s,\"changed_pct\":%.1f},\"present\":{\"person\":1},"
                 "\"events\":{\"stored\":14,\"last_id\":14},\"mqtt\":{\"enabled\":true,\"connected\":true},"
                 "\"system\":{\"soc_temp_c\":%.1f,\"mem_available_mb\":%.1f,\"mem_total_mb\":54.3,\"process_rss_mb\":19.4,\"cpu_system_pct\":%.1f,"
                 "\"cpu_process_pct\":%.1f,\"ip\":\"192.168.1.160\",\"storage_free_mb\":120.5,\"storage_total_mb\":181.3,"
                 "\"flash_free_mb\":137.5,\"flash_total_mb\":181.3,"
                 "\"sd\":{\"present\":true,\"mounted\":true,\"in_use\":true,\"device\":\"/dev/mmcblk1p1\",\"total_mb\":29800.0,\"free_mb\":27150.0}}}",
                 t * 2, (long long)(time(nullptr) * 1000LL), t * 60, t / 5, 2000.0 + 60.0 * sin(t / 3.0), t / 10, t,
                 (t % 6 < 2) ? "true" : "false", (t % 6 < 2) ? 6.5 : 0.4, 62.0 + 4.0 * sin(t / 5.0), 31.0 + 2.0 * cos(t / 4.0),
                 28.0 + 12.0 * sin(t / 4.0), 21.0 + 6.0 * sin(t / 4.0));
        return buf;
    };
    be.detectionsJson = [&]() -> std::string {
        const double s = sin(tick / 6.0) * 0.1;
        char buf[400];
        snprintf(buf, sizeof(buf),
                 "{\"enabled\":true,\"model_loaded\":true,\"error\":\"\",\"time_ms\":0,\"detections\":[{\"label\":\"person\",\"conf\":0.87,\"x1\":%.3f,\"y1\":0.3,\"x2\":%.3f,\"y2\":0.85},"
                 "{\"label\":\"chair\",\"conf\":0.52,\"x1\":0.7,\"y1\":0.55,\"x2\":0.9,\"y2\":0.9}]}",
                 0.35 + s, 0.55 + s);
        return buf;
    };
    be.snapshot = [&](std::vector<uint8_t>* j, std::string*) { *j = jpeg; return true; };
    be.getConfig = [&] { return cfg; };
    be.applyConfig = [&](const aid::Config& c, std::string*, bool*) { cfg = c; return true; };
    be.systemAction = [&](const std::string& a) { aid::logWarn("demo: system action " + a + " ignored"); return true; };
    be.mqttJson = [] { return std::string("{\"enabled\":true,\"connected\":true,\"host\":\"192.168.1.5\",\"port\":1883,\"prefix\":\"luckfox-ai\",\"error\":\"\"}"); };
    be.gpioJson = [] { return std::string("{\"enabled\":false,\"lines\":[]}"); };
    be.systemInfoJson = [] {
        return std::string("{\"aid_version\":\"demo\",\"hostname\":\"luckfox\",\"board\":\"Luckfox Pico Pro Max\",\"kernel\":\"Linux version 5.10.160 (demo)\","
                           "\"os\":\"Buildroot 2023.02.6\",\"ip\":\"192.168.1.160\",\"uptime_s\":93784,\"mem_total_mb\":54.3,\"root_total_mb\":181.3,\"root_free_mb\":137.5,"
                           "\"data_total_mb\":181.3,\"data_free_mb\":120.5,\"rtsp_port\":554,\"http_port\":8080}");
    };

    aid::Api api(&auth, &events, webDir, cfg.httpToken, 8, be);
    aid::HttpServer http;
    if (!http.start(port, false, [&](const aid::HttpRequest& r, aid::HttpResponse* s) { return api.handle(r, s); }, &err)) {
        fprintf(stderr, "cannot start: %s\n", err.c_str());
        return 1;
    }
    printf("demo dashboard on http://127.0.0.1:%d  (login admin / demo-pass-123)\n", port);
    fflush(stdout);
    while (!g_quit) usleep(200 * 1000);
    http.stop();
    std::string cmd = std::string("rm -rf ") + tmpl;
    return system(cmd.c_str());
}
