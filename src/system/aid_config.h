// Runtime configuration for aid-core, read from a simple key=value file.
// No secrets live in the repository: tokens/passwords exist only in the file on the board.
#pragma once

#include <string>
#include <vector>

namespace aid {

// Rectangle in normalized image coordinates (0..1), origin top-left.
struct ZoneRect {
    float x1 = 0, y1 = 0, x2 = 1, y2 = 1;
    ZoneRect() = default;
    ZoneRect(float a, float b, float c, float d) : x1(a), y1(b), x2(c), y2(d) {}
};

// One GPIO line (see src/gpio). Pin numbers are Linux sysfs numbers = bank*32 + group*8 + x.
struct GpioLine {
    std::string name;      // e.g. "relay1"
    std::string role;      // relay, buzzer, led, alarm (outputs); pir, door (inputs)
    int pin = -1;
    bool output = true;
    bool activeLow = false;
};

struct Config {
    // ---- video
    int width = 1920;
    int height = 1080;
    int fps = 30;
    int bitrateKbps = 2048;
    int gop = 60;
    int jpegQuality = 80;
    std::string iqDir = "/etc/iqfiles";
    // image controls, -1 = leave the ISP default
    int brightness = -1;
    int contrast = -1;
    int saturation = -1;
    int sharpness = -1;

    // ---- network
    int rtspPort = 554;
    std::string rtspPath = "/live/0";
    int httpPort = 8080;
    // Token for read-only machine access (/snapshot.jpg, /status.json). Empty = no token access.
    std::string httpToken;

    // ---- storage
    // Events and pictures go to RAM by default (/tmp is a RAM disk): the board's SPI flash showed
    // controller timeouts in testing, so it is not written to at run time. History is lost at reboot.
    // For lasting history point data_dir at a microSD card (e.g. /mnt/sdcard/aid).
    std::string dataDir = "/tmp/aid-data";
    std::string webDir = "/root/aid/web";
    std::string authFile = "/root/aid/auth.json";

    // ---- AI detection
    bool detectEnabled = true;
    int detectIntervalMs = 500;                // time between detections
    int detectConfPct = 45;                    // minimum confidence, percent
    std::string modelPath = "/root/aid/model/yolov5.rknn";
    std::string labelsPath = "/root/aid/model/coco_80_labels_list.txt";
    std::vector<std::string> detectClasses = {"person", "car", "motorcycle", "bicycle",
                                              "bus", "truck", "dog", "cat"};
    std::vector<ZoneRect> zones;               // empty = whole frame

    // ---- events
    int eventCooldownS = 30;                   // minimum seconds between events of one type
    int eventAbsentS = 10;                     // seconds without the object before "disappeared"
    int eventEnterFrames = 2;                  // consecutive detections needed to confirm presence
    bool motionEnabled = true;
    int motionThresholdPct = 3;                // percent of sampled pixels that must change
    int eventsKeep = 300;
    int snapshotsKeep = 30;

    // ---- MQTT / Home Assistant
    bool mqttEnabled = false;
    std::string mqttHost;
    int mqttPort = 1883;
    std::string mqttUser;
    std::string mqttPassword;
    std::string mqttPrefix = "luckfox-ai";
    std::string mqttDeviceName = "Luckfox AI Camera";
    std::string mqttDiscoveryPrefix = "homeassistant";

    // ---- GPIO (disabled by default: pins must be verified for your board first)
    bool gpioEnabled = false;
    std::vector<GpioLine> gpio;

    // ---- security
    int sessionIdleMin = 15;
    int sessionMaxHours = 8;
};

// Parses `key=value` lines ('#' starts a comment). Repeatable keys (zone, gpio) are appended.
// Unknown keys and unparsable values are reported in *problems and ignored; a value that is
// out of its allowed range makes the function return false (and is not applied).
bool parseConfig(const std::string& text, Config* out, std::vector<std::string>* problems);

bool loadConfigFile(const std::string& path, Config* out, std::vector<std::string>* problems,
                    std::string* err);

// Writes the configuration in the same key=value format (comments are not preserved).
std::string serializeConfig(const Config& cfg);

// Writes the file atomically (temp file + rename) with mode 0600.
bool saveConfigFile(const std::string& path, const Config& cfg, std::string* err);

}  // namespace aid
