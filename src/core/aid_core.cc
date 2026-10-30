// aid-core: camera + hardware H.264 RTSP stream + HTTP snapshot/status (Phase 4).
// Later phases add detection, events, MQTT and GPIO to this process.
//
//   aid-core [--config /root/aid/aid.conf]
//
// Stop the stock rkipc first (scripts/stop_stock_camera.sh); it owns the camera.
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "camera.h"
#include "config.h"
#include "http_server.h"
#include "streamer.h"
#include "sysinfo.h"

namespace {

constexpr const char* kVersion = "0.4.0-dev";
volatile sig_atomic_t g_quit = 0;

void onSignal(int) { g_quit = 1; }

std::string statusJson(const aid::Config& cfg, aid::Streamer& streamer) {
    const aid::StreamStats s = streamer.stats();
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(2);
    o << "{\n";
    o << "  \"version\": \"" << kVersion << "\",\n";
    o << "  \"uptime_s\": " << aid::uptimeSeconds() << ",\n";
    o << "  \"camera\": {\"sensor\": \"" << aid::Camera::detectSensorName() << "\", \"width\": "
      << cfg.width << ", \"height\": " << cfg.height << ", \"fps_config\": " << cfg.fps << "},\n";
    o << "  \"stream\": {\"running\": " << (s.running ? "true" : "false")
      << ", \"rtsp_path\": \"" << cfg.rtspPath << "\", \"rtsp_port\": " << cfg.rtspPort
      << ", \"bitrate_kbps_config\": " << cfg.bitrateKbps << ",\n";
    o << "             \"frames\": " << s.frames << ", \"keyframes\": " << s.keyframes
      << ", \"recent_fps\": " << s.recentFps << ", \"recent_kbps\": " << s.recentKbps << ",\n";
    o << "             \"encode_latency_avg_ms\": " << s.latencyAvgMs
      << ", \"encode_latency_max_ms\": " << s.latencyMaxMs << ", \"snapshots\": " << s.snapshots
      << ",\n             \"last_snapshot_ms\": {\"frame_wait\": " << s.snapFrameWaitMs
      << ", \"encode\": " << s.snapEncodeMs << ", \"total\": " << s.snapTotalMs << "}},\n";
    o << "  \"system\": {\"soc_temp_c\": " << aid::readSocTempC()
      << ", \"mem_available_mb\": " << aid::memAvailableMB()
      << ", \"process_rss_mb\": " << aid::processRssMB() << "}\n";
    o << "}\n";
    return o.str();
}

}  // namespace

int main(int argc, char** argv) {
    aid::uptimeSeconds();  // starts the uptime clock
    std::string configPath = "/root/aid/aid.conf";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            configPath = argv[++i];
        } else {
            fprintf(stderr, "usage: %s [--config file]\n", argv[0]);
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
        for (const auto& p : problems) fprintf(stderr, "config warning: %s\n", p.c_str());
    } else {
        fprintf(stderr, "config: %s not found, using defaults (HTTP stays on localhost)\n",
                configPath.c_str());
    }

    // ---- camera (hardware-bound mode)
    aid::Camera camera;
    aid::CameraConfig cc;
    cc.width = cfg.width;
    cc.height = cfg.height;
    cc.fps = cfg.fps;
    cc.iqDir = cfg.iqDir;
    cc.viDepth = 1;  // H.264 encoder is bound; snapshots pull single frames
    cc.controls.brightness = cfg.brightness;
    cc.controls.contrast = cfg.contrast;
    cc.controls.saturation = cfg.saturation;
    cc.controls.sharpness = cfg.sharpness;
    if (!camera.open(cc, &err)) {
        fprintf(stderr, "camera open failed: %s\n", err.c_str());
        return 1;
    }

    // ---- video path
    aid::Streamer streamer;
    aid::StreamConfig sc;
    sc.viDev = camera.viDev();
    sc.viChn = camera.viChn();
    sc.width = cfg.width;
    sc.height = cfg.height;
    sc.fps = cfg.fps;
    sc.bitrateKbps = cfg.bitrateKbps;
    sc.gop = cfg.gop;
    sc.rtspPort = cfg.rtspPort;
    sc.rtspPath = cfg.rtspPath;
    sc.jpegQuality = cfg.jpegQuality;
    if (!streamer.start(sc, &err)) {
        fprintf(stderr, "stream start failed: %s\n", err.c_str());
        camera.close();
        return 1;
    }

    // ---- HTTP: /health, /snapshot.jpg, /status.json
    aid::HttpServer http;
    const bool httpOk = http.start(
        cfg.httpPort, cfg.httpToken,
        [&](const aid::HttpRequest& req, aid::HttpResponse* resp) -> bool {
            if (req.path == "/health") {
                const std::string t = "ok\n";
                resp->body.assign(t.begin(), t.end());
            } else if (req.path == "/status.json") {
                const std::string t = statusJson(cfg, streamer);
                resp->contentType = "application/json";
                resp->body.assign(t.begin(), t.end());
            } else if (req.path == "/snapshot.jpg") {
                std::string e;
                if (streamer.snapshot(&resp->body, &e)) {
                    resp->contentType = "image/jpeg";
                } else {
                    resp->status = 503;
                    const std::string t = "snapshot failed: " + e + "\n";
                    resp->body.assign(t.begin(), t.end());
                }
            } else {
                return false;
            }
            return true;
        },
        &err);
    if (!httpOk) fprintf(stderr, "http start failed: %s (video keeps running)\n", err.c_str());

    printf("aid-core %s running: %dx%d @ %d fps, rtsp://<board>:%d%s", kVersion, cfg.width,
           cfg.height, cfg.fps, cfg.rtspPort, cfg.rtspPath.c_str());
    if (httpOk)
        printf(", http port %d (%s)", cfg.httpPort, http.isLocalOnly() ? "localhost only" : "token required");
    printf("\n");
    fflush(stdout);

    int tick = 0;
    while (!g_quit) {
        sleep(1);
        if (++tick % 10 == 0) {
            const aid::StreamStats s = streamer.stats();
            printf("[%.0fs] fps %.1f, %.0f kbit/s, latency avg %.1f ms, temp %.1f C, mem avail %.0f MB, rss %.1f MB\n",
                   aid::uptimeSeconds(), s.recentFps, s.recentKbps, s.latencyAvgMs,
                   aid::readSocTempC(), aid::memAvailableMB(), aid::processRssMB());
            fflush(stdout);
        }
    }

    printf("stopping\n");
    http.stop();
    streamer.stop();
    camera.close();
    return 0;
}
