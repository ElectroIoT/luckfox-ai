// Runs the detector and motion detection on a timer (Phase 5/6). One thread. At most one camera
// frame is held at a time, and only while it is converted for the model; the NPU runs after the
// frame has been released.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "config.h"
#include "detector.h"
#include "frame_view.h"

namespace aid {

// Calls fn with a frame (valid only during the call); returns false if no frame arrived in time.
using FrameProvider = std::function<bool(int timeoutMs, const std::function<void(const FrameView&)>& fn)>;

struct DetectionStatus {
    bool modelLoaded = false;
    bool enabled = false;
    bool cameraOk = true;
    uint64_t runs = 0;
    double lastPreMs = 0, lastInferMs = 0, lastPostMs = 0, avgInferMs = 0;
    int lastCount = 0;
    double motionPct = 0;
    int64_t lastRunWallMs = 0;
    std::string error;
};

class DetectionService {
public:
    using ResultCb = std::function<void(const std::vector<Detection>& dets, bool moving, int64_t nowMs, int64_t wallMs)>;
    using CameraCb = std::function<void(bool ok, int64_t nowMs, int64_t wallMs)>;

    DetectionService();
    ~DetectionService();
    DetectionService(const DetectionService&) = delete;
    DetectionService& operator=(const DetectionService&) = delete;

    // Loads the model if detection is enabled. If loading fails the service still starts and
    // does motion detection and camera monitoring only; the reason is in status().error.
    bool start(const Config& cfg, FrameProvider provider, ResultCb onResult, CameraCb onCamera);
    void stop();
    void reconfigure(const Config& cfg);   // enable flag, interval, confidence, motion threshold

    DetectionStatus status() const;
    std::vector<Detection> lastDetections() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aid
