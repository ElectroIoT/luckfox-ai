// aid-core: the camera appliance. Camera + hardware H.264 RTSP stream + NPU detection + events +
// web dashboard/API + MQTT/Home Assistant + GPIO, in one process (about 15 threads, small stacks).
//
//   aid-core [--config /root/aid/aid.conf]
//   aid-core --set-password [--user admin] [--config ...]   (reads the new password from stdin)
//
// Stop the stock rkipc first (scripts/stop_stock_camera.sh); it owns the camera.
// Exit codes: 0 = stopped on request, 1 = start-up failure, 2 = bad usage/config, 3 = restart requested.
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "api.h"
#include "auth.h"
#include "camera.h"
#include "aid_config.h"
#include "detection_service.h"
#include "event_engine.h"
#include "event_store.h"
#include "gpio.h"
#include "ha_discovery.h"
#include "http_server.h"
#include "json_util.h"
#include "log.h"
#include "mqtt_client.h"
#include "streamer.h"
#include "sysinfo.h"

namespace {

constexpr const char* kVersion = "1.0.0-rc1";
volatile sig_atomic_t g_quit = 0;
std::atomic<int> g_pending{0};  // 0 none, 1 restart service, 2 reboot, 3 shutdown

void onSignal(int) { g_quit = 1; }

int64_t monoMs() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
int64_t wallMs() {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

std::string q(const std::string& s) { return "\"" + aid::jsonEscape(s) + "\""; }
std::string b(bool v) { return v ? "true" : "false"; }

// CPU usage of the whole system and of this process, sampled by the main loop.
struct CpuSampler {
    unsigned long long lastTotal = 0, lastBusy = 0, lastProc = 0;
    std::atomic<double> systemPct{-1}, processPct{-1};
    void sample() {
        unsigned long long user = 0, nice = 0, sys = 0, idle = 0, iow = 0, irq = 0, sirq = 0, steal = 0;
        FILE* f = fopen("/proc/stat", "r");
        if (!f) return;
        const int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &user, &nice, &sys, &idle, &iow, &irq, &sirq, &steal);
        fclose(f);
        if (n < 4) return;
        const unsigned long long total = user + nice + sys + idle + iow + irq + sirq + steal;
        const unsigned long long busy = total - idle - iow;
        unsigned long long proc = 0;
        FILE* p = fopen("/proc/self/stat", "r");
        if (p) {
            char buf[1024];
            if (fgets(buf, sizeof(buf), p)) {
                const char* rp = strrchr(buf, ')');
                unsigned long long ut = 0, st = 0;
                if (rp && sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) == 2) proc = ut + st;
            }
            fclose(p);
        }
        if (lastTotal != 0 && total > lastTotal) {
            const double dt = (double)(total - lastTotal);
            systemPct = 100.0 * (double)(busy - lastBusy) / dt;
            processPct = 100.0 * (double)(proc - lastProc) / dt;
        }
        lastTotal = total; lastBusy = busy; lastProc = proc;
    }
};

bool readPasswordFromStdin(std::string* out) {
    struct termios old, quiet;
    const bool tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    if (tty) {
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
        fprintf(stderr, "New password (input hidden): ");
    }
    const bool ok = (bool)std::getline(std::cin, *out);
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        fprintf(stderr, "\n");
    }
    while (!out->empty() && (out->back() == '\r' || out->back() == '\n')) out->pop_back();
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    aid::uptimeSeconds();  // starts the uptime clock
    std::string configPath = "/root/aid/aid.conf";
    std::string user = "admin";
    bool setPassword = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) configPath = argv[++i];
        else if (!strcmp(argv[i], "--user") && i + 1 < argc) user = argv[++i];
        else if (!strcmp(argv[i], "--set-password")) setPassword = true;
        else if (!strcmp(argv[i], "--version")) { printf("aid-core %s\n", kVersion); return 0; }
        else {
            fprintf(stderr, "usage: %s [--config file] | --set-password [--user name] [--config file]\n", argv[0]);
            return 2;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    aid::Config cfg;
    std::vector<std::string> problems;
    std::string err;
    if (access(configPath.c_str(), R_OK) == 0) {
        if (!aid::loadConfigFile(configPath, &cfg, &problems, &err)) {
            for (const auto& p : problems) fprintf(stderr, "config: %s\n", p.c_str());
            fprintf(stderr, "config error: %s\n", err.c_str());
            return 2;
        }
        for (const auto& p : problems) aid::logWarn("config: " + p);
    } else {
        aid::logWarn("config file " + configPath + " not found, using defaults");
    }

    // ---- password setup mode (no camera needed)
    if (setPassword) {
        std::string pw;
        if (!readPasswordFromStdin(&pw)) { fprintf(stderr, "no password given\n"); return 2; }
        aid::AuthManager am(cfg.authFile, cfg.sessionIdleMin, cfg.sessionMaxHours);
        if (!am.setPassword(user, pw, &err)) { fprintf(stderr, "cannot set the password: %s\n", err.c_str()); return 2; }
        printf("password for '%s' stored in %s\n", user.c_str(), cfg.authFile.c_str());
        return 0;
    }

    std::mutex cfgMutex;
    auto cfgCopy = [&]() { std::lock_guard<std::mutex> l(cfgMutex); return cfg; };

    // ---- storage, events, auth
    aid::EventStore store(cfg.dataDir, cfg.eventsKeep, cfg.snapshotsKeep);
    if (!store.init(&err)) aid::logError("event storage unavailable: " + err);
    aid::AuthManager auth(cfg.authFile, cfg.sessionIdleMin, cfg.sessionMaxHours);
    if (!auth.load(&err))
        aid::logWarn("no dashboard password set (" + err + "): run 'aid-core --set-password'; the web interface stays on localhost");

    // ---- camera (frames can be pulled one at a time while the H.264 encoder is bound)
    aid::Camera camera;
    aid::CameraConfig cc;
    cc.width = cfg.width; cc.height = cfg.height; cc.fps = cfg.fps; cc.iqDir = cfg.iqDir;
    cc.viDepth = 1;
    cc.controls.brightness = cfg.brightness; cc.controls.contrast = cfg.contrast;
    cc.controls.saturation = cfg.saturation; cc.controls.sharpness = cfg.sharpness;
    if (!camera.open(cc, &err)) { aid::logError("camera open failed: " + err); return 1; }

    aid::Streamer streamer;
    aid::StreamConfig sc;
    sc.viDev = camera.viDev(); sc.viChn = camera.viChn();
    sc.width = cfg.width; sc.height = cfg.height; sc.fps = cfg.fps;
    sc.bitrateKbps = cfg.bitrateKbps; sc.gop = cfg.gop;
    sc.rtspPort = cfg.rtspPort; sc.rtspPath = cfg.rtspPath; sc.jpegQuality = cfg.jpegQuality;
    if (!streamer.start(sc, &err)) {
        aid::logError("stream start failed: " + err);
        camera.close();
        return 1;
    }

    // ---- MQTT (optional)
    aid::MqttClient mqtt;
    aid::HaDevice ha;
    ha.prefix = cfg.mqttPrefix; ha.deviceName = cfg.mqttDeviceName; ha.discoveryPrefix = cfg.mqttDiscoveryPrefix;
    ha.swVersion = kVersion;
    const std::string ip = aid::localIpv4();
    if (!ip.empty()) ha.snapshotUrl = "http://" + ip + ":" + std::to_string(cfg.httpPort) + "/snapshot.jpg";
    bool mqttOn = false;
    std::mutex pubMutex;
    std::map<std::string, std::string> lastPublished;   // topic -> last payload (state topics)
    int64_t lastTempPublishMs = 0;
    auto pub = [&](const std::string& tail, const std::string& payload, bool retain) {
        if (mqttOn) mqtt.publish(aid::haTopic(ha, tail), payload, retain);
    };

    // ---- event engine
    std::mutex engineMutex;
    aid::EventEngine engine(cfg);
    const std::set<std::string> snapTypes = {"person_detected", "vehicle_detected", "animal_detected", "motion_detected",
                                             "object_entered", "input_changed"};
    engine.setSink([&](aid::Event& e) {
        if (cfg.snapshotsKeep > 0 && snapTypes.count(e.type)) {
            std::vector<uint8_t> jpeg;
            std::string se;
            if (streamer.snapshot(&jpeg, &se)) e.snapshot = store.saveSnapshot(e.id, jpeg);
        }
        store.add(e);
        aid::logInfo("event #" + std::to_string(e.id) + " " + e.type + (e.label.empty() ? "" : " (" + e.label + ")"));
        pub("events", aid::EventStore::toJson(e), false);
    });

    // ---- detection + motion
    int64_t lastMotionMs = 0;
    aid::DetectionService detection;
    detection.start(
        cfg,
        [&](int timeoutMs, const std::function<void(const aid::FrameView&)>& fn) { return streamer.withFrame(timeoutMs, fn); },
        [&](const std::vector<aid::Detection>& dets, bool moving, int64_t nowMs, int64_t wall) {
            std::lock_guard<std::mutex> l(engineMutex);
            engine.onDetections(dets, nowMs, wall);
            engine.onMotion(moving, nowMs, wall);
            if (moving) lastMotionMs = nowMs;
        },
        [&](bool ok, int64_t nowMs, int64_t wall) {
            std::lock_guard<std::mutex> l(engineMutex);
            engine.onCameraState(ok, nowMs, wall);
        });

    // ---- GPIO (optional, off by default)
    aid::Gpio gpio;
    bool gpioOn = false;
    if (cfg.gpioEnabled) {
        if (gpio.init(cfg.gpio, &err)) { gpioOn = true; aid::logInfo("GPIO enabled: " + std::to_string(cfg.gpio.size()) + " lines"); }
        else aid::logError("GPIO disabled: " + err);
    }

    CpuSampler cpu;

    // ---- status builders
    auto statusJson = [&]() -> std::string {
        const aid::Config c = cfgCopy();
        const aid::StreamStats s = streamer.stats();
        const aid::DetectionStatus d = detection.status();
        std::map<std::string, int> counts;
        bool motionActive;
        {
            std::lock_guard<std::mutex> l(engineMutex);
            counts = engine.currentCounts();
            motionActive = engine.motionActive();
        }
        double total = 0, freeMb = 0;
        const bool disk = aid::diskUsageMB(c.dataDir, &total, &freeMb);
        std::ostringstream o;
        o.setf(std::ios::fixed);
        o.precision(2);
        o << "{\"version\":" << q(kVersion) << ",\"uptime_s\":" << aid::uptimeSeconds() << ",\"time_ms\":" << wallMs() << ",\n";
        o << " \"camera\":{\"sensor\":" << q(aid::Camera::detectSensorName()) << ",\"ok\":" << b(d.cameraOk)
          << ",\"width\":" << c.width << ",\"height\":" << c.height << ",\"fps_config\":" << c.fps << "},\n";
        o << " \"stream\":{\"running\":" << b(s.running) << ",\"rtsp_port\":" << c.rtspPort << ",\"rtsp_path\":" << q(c.rtspPath)
          << ",\"bitrate_kbps_config\":" << c.bitrateKbps << ",\"frames\":" << s.frames << ",\"keyframes\":" << s.keyframes
          << ",\"recent_fps\":" << s.recentFps << ",\"recent_kbps\":" << s.recentKbps << ",\"encode_latency_avg_ms\":"
          << s.latencyAvgMs << ",\"encode_latency_max_ms\":" << s.latencyMaxMs << ",\"snapshots\":" << s.snapshots
          << ",\"last_snapshot_ms\":{\"frame_wait\":" << s.snapFrameWaitMs << ",\"encode\":" << s.snapEncodeMs
          << ",\"total\":" << s.snapTotalMs << "}},\n";
        o << " \"ai\":{\"enabled\":" << b(d.enabled) << ",\"model_loaded\":" << b(d.modelLoaded) << ",\"runs\":" << d.runs
          << ",\"interval_ms\":" << c.detectIntervalMs << ",\"conf_pct\":" << c.detectConfPct << ",\"last_pre_ms\":"
          << d.lastPreMs << ",\"last_infer_ms\":" << d.lastInferMs << ",\"last_post_ms\":" << d.lastPostMs
          << ",\"avg_infer_ms\":" << d.avgInferMs << ",\"last_count\":" << d.lastCount << ",\"error\":" << q(d.error) << "},\n";
        o << " \"motion\":{\"enabled\":" << b(c.motionEnabled) << ",\"active\":" << b(motionActive) << ",\"changed_pct\":"
          << d.motionPct << "},\n";
        o << " \"present\":{";
        bool first = true;
        for (const auto& kv : counts) { o << (first ? "" : ",") << q(kv.first) << ":" << kv.second; first = false; }
        o << "},\n";
        o << " \"events\":{\"stored\":" << store.count() << ",\"last_id\":" << engine.lastEventId() << "},\n";
        o << " \"mqtt\":{\"enabled\":" << b(c.mqttEnabled) << ",\"connected\":" << b(mqttOn && mqtt.connected()) << "},\n";
        o << " \"system\":{\"soc_temp_c\":" << aid::readSocTempC() << ",\"mem_available_mb\":" << aid::memAvailableMB()
          << ",\"mem_total_mb\":" << aid::memTotalMB() << ",\"process_rss_mb\":" << aid::processRssMB()
          << ",\"cpu_system_pct\":" << cpu.systemPct.load() << ",\"cpu_process_pct\":" << cpu.processPct.load()
          << ",\"ip\":" << q(aid::localIpv4()) << ",\"storage_free_mb\":" << (disk ? freeMb : -1)
          << ",\"storage_total_mb\":" << (disk ? total : -1) << "}}\n";
        return o.str();
    };

    aid::ApiBackend be;
    be.statusJson = statusJson;
    be.snapshot = [&](std::vector<uint8_t>* j, std::string* e) { return streamer.snapshot(j, e); };
    be.detectionsJson = [&]() -> std::string {
        const std::vector<aid::Detection> dets = detection.lastDetections();
        const aid::DetectionStatus d = detection.status();
        std::string o = "{\"enabled\":" + b(d.enabled) + ",\"model_loaded\":" + b(d.modelLoaded) +
                        ",\"error\":" + q(d.error) + ",\"time_ms\":" + std::to_string((long long)d.lastRunWallMs) + ",\"detections\":[";
        for (size_t i = 0; i < dets.size(); ++i) {
            char buf[200];
            snprintf(buf, sizeof(buf), "{\"label\":%s,\"conf\":%.3f,\"x1\":%.4f,\"y1\":%.4f,\"x2\":%.4f,\"y2\":%.4f}",
                     q(dets[i].label).c_str(), dets[i].conf, dets[i].x1, dets[i].y1, dets[i].x2, dets[i].y2);
            o += (i ? "," : "") + std::string(buf);
        }
        return o + "]}";
    };
    be.mqttJson = [&]() -> std::string {
        const aid::Config c = cfgCopy();
        return "{\"enabled\":" + b(c.mqttEnabled) + ",\"connected\":" + b(mqttOn && mqtt.connected()) + ",\"host\":" +
               q(c.mqttHost) + ",\"port\":" + std::to_string(c.mqttPort) + ",\"prefix\":" + q(c.mqttPrefix) +
               ",\"error\":" + q(mqttOn ? mqtt.lastError() : "") + "}";
    };
    be.gpioJson = [&]() -> std::string {
        std::string o = "{\"enabled\":" + b(gpioOn) + ",\"lines\":[";
        bool first = true;
        for (const aid::GpioLine& l : gpio.lines()) {
            bool st = false;
            gpio.getState(l.name, &st);
            o += std::string(first ? "" : ",") + "{\"name\":" + q(l.name) + ",\"role\":" + q(l.role) + ",\"pin\":" +
                 std::to_string(l.pin) + ",\"output\":" + b(l.output) + ",\"active\":" + b(st) + "}";
            first = false;
        }
        return o + "]}";
    };
    be.gpioSet = [&](const std::string& name, bool active, std::string* e) {
        if (!gpioOn) { *e = "GPIO is not enabled"; return false; }
        return gpio.setOutput(name, active, e);
    };
    be.getConfig = cfgCopy;
    be.applyConfig = [&](const aid::Config& next, std::string* e, bool*) {
        if (!aid::saveConfigFile(configPath, next, e)) return false;
        {
            std::lock_guard<std::mutex> l(cfgMutex);
            cfg = next;
        }
        detection.reconfigure(next);
        {
            std::lock_guard<std::mutex> l(engineMutex);
            engine.reconfigure(next);
        }
        aid::ImageControls ic;
        ic.brightness = next.brightness; ic.contrast = next.contrast; ic.saturation = next.saturation; ic.sharpness = next.sharpness;
        std::string ce;
        if (!camera.setControls(ic, &ce)) aid::logWarn("image controls not applied: " + ce);
        return true;
    };
    be.systemAction = [&](const std::string& a) {
        g_pending = a == "restart_service" ? 1 : (a == "reboot" ? 2 : 3);
        g_quit = 1;
        return true;
    };
    be.systemInfoJson = [&]() -> std::string {
        double rt = 0, rf = 0, dt = 0, df = 0;
        const bool r = aid::diskUsageMB("/", &rt, &rf), d = aid::diskUsageMB(cfgCopy().dataDir, &dt, &df);
        std::ostringstream o;
        o.setf(std::ios::fixed);
        o.precision(1);
        o << "{\"aid_version\":" << q(kVersion) << ",\"hostname\":" << q(aid::firstLine("/etc/hostname"))
          << ",\"board\":" << q(aid::firstLine("/proc/device-tree/model")) << ",\"kernel\":" << q(aid::firstLine("/proc/version"))
          << ",\"os\":" << q(aid::keyValueFromFile("/etc/os-release", "PRETTY_NAME")) << ",\"ip\":" << q(aid::localIpv4())
          << ",\"uptime_s\":" << aid::systemUptimeSeconds() << ",\"mem_total_mb\":" << aid::memTotalMB()
          << ",\"root_total_mb\":" << (r ? rt : -1) << ",\"root_free_mb\":" << (r ? rf : -1)
          << ",\"data_total_mb\":" << (d ? dt : -1) << ",\"data_free_mb\":" << (d ? df : -1)
          << ",\"rtsp_port\":" << cfgCopy().rtspPort << ",\"http_port\":" << cfgCopy().httpPort << "}";
        return o.str();
    };

    aid::Api api(&auth, &store, cfg.webDir, cfg.httpToken, cfg.sessionMaxHours, be);
    aid::HttpServer http;
    const bool bindAll = auth.hasPassword() || !cfg.httpToken.empty();
    const bool httpOk = http.start(cfg.httpPort, bindAll,
                                   [&](const aid::HttpRequest& req, aid::HttpResponse* resp) { return api.handle(req, resp); }, &err);
    if (!httpOk) aid::logError("http start failed: " + err + " (video keeps running)");

    if (cfg.mqttEnabled && !cfg.mqttHost.empty()) {
        aid::MqttOptions mo;
        mo.host = cfg.mqttHost; mo.port = cfg.mqttPort; mo.user = cfg.mqttUser; mo.password = cfg.mqttPassword;
        mo.clientId = "luckfox-ai-" + aid::haNodeId(cfg.mqttPrefix);
        mo.willTopic = aid::haStatusTopic(ha); mo.willPayload = "offline";
        mqtt.setOnConnect([&] {
            mqtt.publish(aid::haStatusTopic(ha), "online", true);
            for (const aid::HaMessage& m : aid::haDiscoveryMessages(ha)) mqtt.publish(m.topic, m.payload, m.retain);
            if (!ha.snapshotUrl.empty()) mqtt.publish(aid::haTopic(ha, "camera/snapshot_url"), ha.snapshotUrl, true);
            std::lock_guard<std::mutex> l(pubMutex);
            lastPublished.clear();   // state topics are re-sent by the next tick
        });
        if (mqtt.start(mo, &err)) mqttOn = true;
        else aid::logError("MQTT not started: " + err);
    }

    {
        std::lock_guard<std::mutex> l(engineMutex);
        engine.onSystemState(true, monoMs(), wallMs());
    }
    aid::logInfo(std::string("aid-core ") + kVersion + " running: " + std::to_string(cfg.width) + "x" + std::to_string(cfg.height) +
                 " @ " + std::to_string(cfg.fps) + " fps, rtsp port " + std::to_string(cfg.rtspPort) + cfg.rtspPath +
                 (httpOk ? ", web port " + std::to_string(cfg.httpPort) + (http.isLocalOnly() ? " (localhost only)" : "") : ""));

    // ---- main loop: 100 ms tick
    int tick = 0;
    while (!g_quit) {
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, nullptr);
        ++tick;

        if (gpioOn) {
            for (const auto& ch : gpio.pollInputs(monoMs())) {
                std::lock_guard<std::mutex> l(engineMutex);
                engine.onInput(ch.first, ch.second, monoMs(), wallMs());
            }
        }
        if (tick % 10 != 0) continue;

        // once a second
        if (FILE* hb = fopen("/tmp/aid-core.heartbeat", "w")) { fprintf(hb, "%lld\n", (long long)monoMs()); fclose(hb); }
        if (tick % 20 == 0) cpu.sample();

        if (mqttOn && mqtt.connected()) {
            const aid::DetectionStatus d = detection.status();
            std::map<std::string, int> counts;
            bool motionActive;
            {
                std::lock_guard<std::mutex> l(engineMutex);
                counts = engine.currentCounts();
                motionActive = engine.motionActive();
            }
            const int64_t now = monoMs();
            std::map<std::string, std::string> want;
            want["detection/person"] = counts.count("person") ? "ON" : "OFF";
            want["detection/vehicle"] = counts.count("vehicle") ? "ON" : "OFF";
            want["detection/animal"] = counts.count("animal") ? "ON" : "OFF";
            want["detection/motion"] = (motionActive || (lastMotionMs && now - lastMotionMs < 30000)) ? "ON" : "OFF";
            want["camera/status"] = d.cameraOk ? "online" : "disconnected";
            want["ai/status"] = !d.enabled ? "disabled" : (d.modelLoaded ? "running" : "model_error");
            int total = 0;
            for (const auto& kv : counts) total += kv.second;
            want["ai/count"] = std::to_string(total);
            std::lock_guard<std::mutex> l(pubMutex);
            for (const auto& kv : want) {
                auto it = lastPublished.find(kv.first);
                if (it == lastPublished.end() || it->second != kv.second) {
                    pub(kv.first, kv.second, true);
                    lastPublished[kv.first] = kv.second;
                }
            }
            if (now - lastTempPublishMs >= 30000) {
                char t[24];
                snprintf(t, sizeof(t), "%.1f", aid::readSocTempC());
                pub("system/temperature", t, true);
                lastTempPublishMs = now;
            }
        }

        if (tick % 600 == 0) {   // once a minute
            const aid::StreamStats s = streamer.stats();
            const aid::DetectionStatus d = detection.status();
            char line[200];
            snprintf(line, sizeof(line), "stats: stream %.1f fps %.0f kbit/s | ai runs %llu avg %.0f ms | cpu %.0f%% | temp %.1f C | mem %.0f MB free | rss %.1f MB",
                     s.recentFps, s.recentKbps, (unsigned long long)d.runs, d.avgInferMs, cpu.systemPct.load(), aid::readSocTempC(),
                     aid::memAvailableMB(), aid::processRssMB());
            aid::logInfo(line);
        }
    }

    // ---- shutdown in reverse order
    aid::logInfo("stopping");
    {
        std::lock_guard<std::mutex> l(engineMutex);
        engine.onSystemState(false, monoMs(), wallMs());
    }
    http.stop();
    detection.stop();
    if (mqttOn) {
        mqtt.publish(aid::haStatusTopic(ha), "offline", true);
        struct timespec ts = {0, 400 * 1000 * 1000};
        nanosleep(&ts, nullptr);   // let the queued messages go out
        mqtt.stop();
    }
    if (gpioOn) gpio.shutdown();
    streamer.stop();
    camera.close();

    const int action = g_pending.load();
    if (action == 2) { if (system("/bin/busybox reboot") != 0) aid::logError("reboot command failed"); }
    if (action == 3) { if (system("/bin/busybox poweroff") != 0) aid::logError("poweroff command failed"); }
    return action == 1 ? 3 : 0;
}
