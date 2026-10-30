#include "event_engine.h"

#include <algorithm>

namespace aid {

std::string eventGroupOf(const std::string& label) {
    if (label == "person") return "person";
    if (label == "car" || label == "motorcycle" || label == "bus" || label == "truck") return "vehicle";
    if (label == "dog" || label == "cat") return "animal";
    return label;
}

bool detectionInZones(const Detection& d, const std::vector<ZoneRect>& zones) {
    if (zones.empty()) return true;
    const float cx = (d.x1 + d.x2) / 2.0f, cy = (d.y1 + d.y2) / 2.0f;
    for (const ZoneRect& z : zones)
        if (cx >= z.x1 && cx <= z.x2 && cy >= z.y1 && cy <= z.y2) return true;
    return false;
}

EventEngine::EventEngine(const Config& cfg) : cfg_(cfg) {}

void EventEngine::reconfigure(const Config& cfg) { cfg_ = cfg; }

void EventEngine::emit(const std::string& type, const std::string& label, float conf, int count,
                       const std::string& detail, int64_t nowMs, int64_t wallMs, bool useCooldown,
                       const std::string& cooldownKey) {
    if (useCooldown) {
        const std::string key = type + ":" + cooldownKey;
        auto it = lastEmitMs_.find(key);
        if (it != lastEmitMs_.end() && nowMs - it->second < (int64_t)cfg_.eventCooldownS * 1000) return;
        lastEmitMs_[key] = nowMs;
    }
    Event e;
    e.id = nextId_++;
    e.tsMs = wallMs;
    e.type = type;
    e.label = label;
    e.confidence = conf;
    e.count = count;
    e.detail = detail;
    if (sink_) sink_(e);
}

void EventEngine::onDetections(const std::vector<Detection>& dets, int64_t nowMs, int64_t wallMs) {
    // Count confirmed-class detections per group, inside the zones and above the threshold.
    std::map<std::string, Presence> seen;
    for (const Detection& d : dets) {
        if (d.conf * 100.0f < (float)cfg_.detectConfPct) continue;
        if (std::find(cfg_.detectClasses.begin(), cfg_.detectClasses.end(), d.label) == cfg_.detectClasses.end())
            continue;
        if (!detectionInZones(d, cfg_.zones)) continue;
        Presence& p = seen[eventGroupOf(d.label)];
        p.count++;
        if (d.conf > p.bestConf) { p.bestConf = d.conf; p.label = d.label; }
    }

    // Update state for groups seen now.
    for (auto& kv : seen) {
        Presence& st = presence_[kv.first];
        st.consecutive++;
        st.lastSeenMs = nowMs;
        st.count = kv.second.count;
        st.bestConf = kv.second.bestConf;
        st.label = kv.second.label;
        if (!st.present && st.consecutive >= cfg_.eventEnterFrames) {
            st.present = true;
            const std::string& g = kv.first;
            if (g == "person" || g == "vehicle" || g == "animal") {
                emit(g + "_detected", st.label, st.bestConf, st.count, "", nowMs, wallMs, true, g);
            }
            emit("object_entered", st.label, st.bestConf, st.count, "group " + g, nowMs, wallMs, true, g);
        }
    }

    // Groups not seen in this pass.
    for (auto& kv : presence_) {
        if (seen.count(kv.first)) continue;
        Presence& st = kv.second;
        st.consecutive = 0;
        st.count = 0;
        if (st.present && nowMs - st.lastSeenMs >= (int64_t)cfg_.eventAbsentS * 1000) {
            st.present = false;
            emit("object_disappeared", st.label, 0, 0, "group " + kv.first, nowMs, wallMs, false, kv.first);
        }
    }
}

void EventEngine::onMotion(bool moving, int64_t nowMs, int64_t wallMs) {
    if (!cfg_.motionEnabled) { motionActive_ = false; return; }
    if (moving && !motionActive_) emit("motion_detected", "", 0, 0, "", nowMs, wallMs, true, "motion");
    motionActive_ = moving;
}

void EventEngine::onCameraState(bool ok, int64_t nowMs, int64_t wallMs) {
    if (ok == cameraOk_) return;
    cameraOk_ = ok;
    emit(ok ? "camera_reconnected" : "camera_disconnected", "camera", 0, 0, "", nowMs, wallMs, false, "camera");
}

void EventEngine::onSystemState(bool online, int64_t nowMs, int64_t wallMs) {
    if (systemKnown_ && online == systemOnline_) return;
    systemKnown_ = true;
    systemOnline_ = online;
    emit(online ? "system_online" : "system_offline", "system", 0, 0, "", nowMs, wallMs, false, "system");
}

void EventEngine::onInput(const std::string& name, bool active, int64_t nowMs, int64_t wallMs) {
    emit("input_changed", name, 0, active ? 1 : 0, active ? "active" : "inactive", nowMs, wallMs, true,
         name + (active ? ":1" : ":0"));
}

std::map<std::string, int> EventEngine::currentCounts() const {
    std::map<std::string, int> out;
    for (const auto& kv : presence_)
        if (kv.second.present) out[kv.first] = kv.second.count;
    return out;
}

bool EventEngine::groupPresent(const std::string& group) const {
    auto it = presence_.find(group);
    return it != presence_.end() && it->second.present;
}

}  // namespace aid
