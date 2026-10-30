// Camera service for the Luckfox Pico Pro (RV1106) – ISP + VI + hardware JPEG.
// Status: compiles for the board; see docs/hardware.md for what was tested on hardware.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace aid {

// Image adjustments. -1 means "leave as the ISP tuning file set it". Range for the four
// levels is 0..255 (Rockchip rkaiq documentation); the default level is not assumed.
struct ImageControls {
    int brightness = -1;
    int contrast = -1;
    int saturation = -1;
    int sharpness = -1;
    int aeLock = -1;                  // -1 leave, 0 unlock, 1 lock auto exposure
};

struct CameraConfig {
    int width = 1920;
    int height = 1080;
    int fps = 0;                      // 0 = leave the sensor/ISP default
    std::string iqDir = "/etc/iqfiles";  // ISP tuning files
    ImageControls controls;           // applied right after the ISP starts
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

    // Changes image controls while running. Returns false and sets *err on the first failure.
    bool setControls(const ImageControls& c, std::string* err);

    // Current values read back from the ISP as JSON (brightness, contrast, saturation,
    // sharpness, exposure mode, exposure time range in seconds, gain range).
    std::string controlsJson();

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
