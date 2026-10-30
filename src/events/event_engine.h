// Event engine (Phase 6): turns detections, motion and camera state into events with
// confirmation, cooldown and duplicate prevention. Pure logic: no I/O, testable on the PC.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "aid_config.h"

namespace aid {

struct Detection {
    int cls = -1;
    std::string label;
    float conf = 0;
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;  // normalized 0..1 in the full camera frame
};

struct Event {
    uint64_t id = 0;
    int64_t tsMs = 0;          // wall clock, ms since the Unix epoch
    std::string type;          // person_detected, vehicle_detected, animal_detected, motion_detected,
                               // object_entered, object_disappeared, camera_disconnected,
                               // camera_reconnected, system_online, system_offline, input_changed
    std::string label;         // object class or source name
    float confidence = 0;      // 0..1, 0 if not applicable
    int count = 0;             // objects of that group in view
    std::string snapshot;      // file name inside the snapshots directory, "" if none
    std::string detail;
};

// Groups COCO labels: person -> "person"; car/motorcycle/bus/truck -> "vehicle";
// dog/cat -> "animal"; others (bicycle, ...) -> the label itself.
std::string eventGroupOf(const std::string& label);

bool detectionInZones(const Detection& d, const std::vector<ZoneRect>& zones);

class EventEngine {
public:
    using Sink = std::function<void(Event&)>;  // may fill in snapshot; id/ts are already set

    explicit EventEngine(const Config& cfg);
    void setSink(Sink sink) { sink_ = std::move(sink); }
    void reconfigure(const Config& cfg);

    // Call after every detection pass (also with an empty list). nowMs is a monotonic ms clock,
    // wallMs the wall clock for the event timestamps.
    void onDetections(const std::vector<Detection>& dets, int64_t nowMs, int64_t wallMs);
    void onMotion(bool moving, int64_t nowMs, int64_t wallMs);
    void onCameraState(bool ok, int64_t nowMs, int64_t wallMs);
    void onSystemState(bool online, int64_t nowMs, int64_t wallMs);
    void onInput(const std::string& name, bool active, int64_t nowMs, int64_t wallMs);

    // Current number of confirmed objects per group (for the dashboard / MQTT).
    std::map<std::string, int> currentCounts() const;
    bool groupPresent(const std::string& group) const;
    uint64_t lastEventId() const { return nextId_ - 1; }
    bool motionActive() const { return motionActive_; }

private:
    struct Presence {
        int consecutive = 0;
        bool present = false;
        int64_t lastSeenMs = 0;
        int count = 0;
        float bestConf = 0;
        std::string label;
    };

    void emit(const std::string& type, const std::string& label, float conf, int count,
              const std::string& detail, int64_t nowMs, int64_t wallMs, bool useCooldown,
              const std::string& cooldownKey);

    Config cfg_;
    Sink sink_;
    uint64_t nextId_ = 1;
    std::map<std::string, Presence> presence_;       // key: group
    std::map<std::string, int64_t> lastEmitMs_;      // cooldown key -> time
    bool motionActive_ = false;
    bool cameraOk_ = true;
    bool systemOnline_ = false;
    bool systemKnown_ = false;
};

}  // namespace aid
