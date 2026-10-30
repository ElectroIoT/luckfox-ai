// Camera service for the Luckfox Pico Pro (RV1106) – ISP + VI + hardware JPEG.
// Status: compiles for the board; see docs/hardware.md for what was tested on hardware.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace aid {

struct CameraConfig {
    int width = 1920;
    int height = 1080;
    int fps = 0;                      // 0 = leave the sensor/ISP default
    std::string iqDir = "/etc/iqfiles";  // ISP tuning files
};

struct CameraStats {
    uint64_t frames = 0;              // frames received
    double seconds = 0.0;             // time between first and last frame
    double fps = 0.0;                 // (frames-1)/seconds, 0 if fewer than 2 frames
    uint32_t width = 0;
    uint32_t height = 0;
};

struct SnapshotInfo {
    uint32_t bytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    double encodeMs = 0.0;            // SendFrame -> GetStream
};

class Camera {
public:
    Camera();
    ~Camera();
    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;

    // Starts ISP (3A), RKMPI and the VI channel. The camera can be owned by one
    // program at a time: stop the stock rkipc first (scripts/stop_stock_camera.sh).
    bool open(const CameraConfig& cfg, std::string* err);
    void close();
    bool isOpen() const;

    // Receives frames for `seconds` and reports the measured frame rate.
    bool measureFps(double seconds, CameraStats* out, std::string* err);

    // Grabs one frame and writes it as JPEG (hardware encoder).
    bool snapshotJpeg(const std::string& path, int quality, SnapshotInfo* out, std::string* err);

    // Read-only status; does NOT open the camera, so it works while rkipc runs.
    // Sensor name and device nodes are discovered from sysfs, never assumed.
    static std::string probeStatusJson();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aid
