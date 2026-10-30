// Home Assistant MQTT discovery messages (Phase 8). Pure string building, testable on the PC.
#pragma once

#include <string>
#include <vector>

namespace aid {

struct HaDevice {
    std::string prefix = "luckfox-ai";           // topic prefix, e.g. luckfox-ai
    std::string deviceName = "Luckfox AI Camera";
    std::string discoveryPrefix = "homeassistant";
    std::string swVersion = "0.0";
    std::string snapshotUrl;                     // optional, shown as a diagnostic sensor
};

struct HaMessage {
    std::string topic;
    std::string payload;
    bool retain;
};

// Lower-case letters, digits and underscore only (used in discovery topics and unique ids).
std::string haNodeId(const std::string& prefix);

std::string haStatusTopic(const HaDevice& d);                    // <prefix>/status  (online/offline)
std::string haTopic(const HaDevice& d, const std::string& tail); // <prefix>/<tail>

// Retained discovery config messages for all entities.
std::vector<HaMessage> haDiscoveryMessages(const HaDevice& d);

}  // namespace aid
