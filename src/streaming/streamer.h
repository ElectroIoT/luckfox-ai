// Video path for aid-core (Phase 4): VI (camera) --hardware bind--> H.264 encoder --> RTSP.
// Snapshots: one frame is pulled from the same VI channel and sent to a hardware JPEG
// encoder (a second hardware bind of one VI channel is refused by RKMPI, error 0xa0028008).
// No CPU copies of frames. Requires a Camera opened with viDepth = 1.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aid {

struct StreamConfig {
    int viDev = 0;
    int viChn = 0;
    int width = 1920;
    int height = 1080;
    int fps = 30;
    int bitrateKbps = 2048;
    int gop = 60;
    int rtspPort = 554;
    std::string rtspPath = "/live/0";
    int jpegQuality = 80;
};

struct StreamStats {
    bool running = false;
    uint64_t frames = 0;        // encoded H.264 frames since start
    uint64_t bytes = 0;         // encoded bytes since start
    uint64_t keyframes = 0;
    double recentFps = 0.0;     // encoded frames per second over the last ~2 s window
    double recentKbps = 0.0;    // encoded kbit/s over the same window
    double latencyAvgMs = -1;   // capture timestamp -> encoded, averaged over the window; -1 if unavailable
    double latencyMaxMs = -1;
    uint64_t snapshots = 0;
};

class Streamer {
public:
    Streamer();
    ~Streamer();
    Streamer(const Streamer&) = delete;
    Streamer& operator=(const Streamer&) = delete;

    bool start(const StreamConfig& cfg, std::string* err);
    void stop();
    bool running() const;
    StreamStats stats() const;

    // Takes one JPEG from the hardware encoder. Thread-safe (one snapshot at a time).
    bool snapshot(std::vector<uint8_t>* jpeg, std::string* err);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aid
