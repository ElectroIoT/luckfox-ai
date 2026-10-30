#include "detection_service.h"

#include <time.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "log.h"
#include "motion.h"

namespace aid {

namespace {
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
}  // namespace

struct DetectionService::Impl {
    Detector detector;
    MotionDetector motion;
    FrameProvider provider;
    ResultCb onResult;
    CameraCb onCamera;

    mutable std::mutex mutex;   // guards cfg, status, last
    Config cfg;
    DetectionStatus st;
    std::vector<Detection> last;

    std::atomic<bool> quit{false};
    std::thread thread;
    int failedFrames = 0;
    bool cameraOk = true;
    double inferSum = 0;

    void loop() {
        while (!quit) {
            const int64_t cycleStart = monoMs();
            Config c;
            {
                std::lock_guard<std::mutex> lock(mutex);
                c = cfg;
            }
            const bool detect = c.detectEnabled && detector.ready();
            DetectTiming tm;
            bool moving = false;
            float motionPct = 0;
            bool prepared = false;

            const bool got = provider(2000, [&](const FrameView& f) {
                if (detect) prepared = detector.preprocess(f, &tm);
                if (c.motionEnabled) moving = motion.process(f.y, f.width, f.height, f.stride, c.motionThresholdPct, &motionPct);
            });
            const int64_t now = monoMs(), wall = wallMs();

            if (!got) {
                if (++failedFrames == 3 && cameraOk) {
                    cameraOk = false;
                    logWarn("no camera frames for several seconds");
                    if (onCamera) onCamera(false, now, wall);
                }
                { std::lock_guard<std::mutex> lock(mutex); st.cameraOk = cameraOk; }
            } else {
                failedFrames = 0;
                if (!cameraOk) {
                    cameraOk = true;
                    logInfo("camera frames are back");
                    if (onCamera) onCamera(true, now, wall);
                }
                std::vector<Detection> dets;
                std::string err;
                if (detect && prepared) {
                    if (!detector.infer((float)c.detectConfPct / 100.0f, &dets, &tm, &err)) {
                        logWarn("detection failed: " + err);
                        std::lock_guard<std::mutex> lock(mutex);
                        st.error = err;
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    st.cameraOk = true;
                    st.motionPct = motionPct;
                    if (detect && prepared) {
                        st.runs++;
                        st.lastPreMs = tm.preprocessMs;
                        st.lastInferMs = tm.inferMs;
                        st.lastPostMs = tm.postMs;
                        inferSum += tm.inferMs;
                        st.avgInferMs = inferSum / (double)st.runs;
                        st.lastCount = (int)dets.size();
                        st.lastRunWallMs = wall;
                    }
                    last = dets;
                }
                if (onResult) onResult(dets, moving, now, wall);
            }

            // Wait until the next cycle (detection off: still look at the camera twice a second).
            const int interval = c.detectEnabled ? c.detectIntervalMs : 500;
            while (!quit && monoMs() - cycleStart < interval) {
                struct timespec ts = {0, 20 * 1000 * 1000};
                nanosleep(&ts, nullptr);
            }
        }
    }
};

DetectionService::DetectionService() : impl_(new Impl) {}
DetectionService::~DetectionService() { stop(); }

bool DetectionService::start(const Config& cfg, FrameProvider provider, ResultCb onResult, CameraCb onCamera) {
    Impl& m = *impl_;
    if (m.thread.joinable()) return false;
    m.cfg = cfg;
    m.provider = std::move(provider);
    m.onResult = std::move(onResult);
    m.onCamera = std::move(onCamera);
    m.st = DetectionStatus();
    m.st.enabled = cfg.detectEnabled;
    if (cfg.detectEnabled) {
        std::string err;
        if (m.detector.init(cfg.modelPath, cfg.labelsPath, &err)) {
            m.st.modelLoaded = true;
            logInfo("AI model loaded: " + cfg.modelPath);
        } else {
            m.st.error = err;
            logError(err + " - running without AI detection");
        }
    }
    m.quit = false;
    m.thread = std::thread([&m] { m.loop(); });
    return true;
}

void DetectionService::stop() {
    Impl& m = *impl_;
    m.quit = true;
    if (m.thread.joinable()) m.thread.join();
}

void DetectionService::reconfigure(const Config& cfg) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->cfg = cfg;
    impl_->st.enabled = cfg.detectEnabled;
}

DetectionStatus DetectionService::status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->st;
}

std::vector<Detection> DetectionService::lastDetections() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->last;
}

}  // namespace aid
