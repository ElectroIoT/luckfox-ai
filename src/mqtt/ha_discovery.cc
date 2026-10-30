#include "ha_discovery.h"

#include "json_util.h"

namespace aid {

std::string haNodeId(const std::string& prefix) {
    std::string o;
    for (char c : prefix) {
        if (c >= 'A' && c <= 'Z') o += (char)(c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) o += c;
        else o += '_';
    }
    return o.empty() ? "luckfox_ai" : o;
}

std::string haStatusTopic(const HaDevice& d) { return d.prefix + "/status"; }
std::string haTopic(const HaDevice& d, const std::string& tail) { return d.prefix + "/" + tail; }

namespace {

std::string q(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }

std::string deviceBlock(const HaDevice& d) {
    return "\"device\":{\"identifiers\":[" + q(haNodeId(d.prefix)) + "],\"name\":" + q(d.deviceName) +
           ",\"manufacturer\":\"Luckfox\",\"model\":\"Pico Pro (RV1106G2)\",\"sw_version\":" + q(d.swVersion) + "}";
}

HaMessage entity(const HaDevice& d, const std::string& component, const std::string& object,
                 const std::string& name, const std::string& stateTail, const std::string& extra) {
    const std::string node = haNodeId(d.prefix);
    HaMessage m;
    m.topic = d.discoveryPrefix + "/" + component + "/" + node + "/" + object + "/config";
    m.retain = true;
    m.payload = "{\"name\":" + q(name) + ",\"unique_id\":" + q(node + "_" + object) +
                ",\"state_topic\":" + q(haTopic(d, stateTail)) +
                ",\"availability_topic\":" + q(haStatusTopic(d)) +
                ",\"payload_available\":\"online\",\"payload_not_available\":\"offline\"" + extra + "," +
                deviceBlock(d) + "}";
    return m;
}

}  // namespace

std::vector<HaMessage> haDiscoveryMessages(const HaDevice& d) {
    const std::string onoff = ",\"payload_on\":\"ON\",\"payload_off\":\"OFF\"";
    std::vector<HaMessage> v;
    v.push_back(entity(d, "binary_sensor", "person", "Person detected", "detection/person",
                       onoff + ",\"device_class\":\"occupancy\""));
    v.push_back(entity(d, "binary_sensor", "vehicle", "Vehicle detected", "detection/vehicle",
                       onoff + ",\"icon\":\"mdi:car\""));
    v.push_back(entity(d, "binary_sensor", "animal", "Animal detected", "detection/animal",
                       onoff + ",\"icon\":\"mdi:paw\""));
    v.push_back(entity(d, "binary_sensor", "motion", "Motion detected", "detection/motion",
                       onoff + ",\"device_class\":\"motion\""));
    v.push_back(entity(d, "sensor", "camera_status", "Camera status", "camera/status", ",\"icon\":\"mdi:cctv\""));
    v.push_back(entity(d, "sensor", "ai_status", "AI status", "ai/status", ",\"icon\":\"mdi:brain\""));
    v.push_back(entity(d, "sensor", "detection_count", "Detection count", "ai/count",
                       ",\"state_class\":\"measurement\",\"icon\":\"mdi:counter\""));
    v.push_back(entity(d, "sensor", "soc_temperature", "SoC temperature", "system/temperature",
                       ",\"device_class\":\"temperature\",\"unit_of_measurement\":\"\\u00b0C\",\"state_class\":\"measurement\""));
    v.push_back(entity(d, "sensor", "last_event", "Last event", "events",
                       ",\"value_template\":\"{{ value_json.type }}\",\"json_attributes_topic\":" +
                           q(haTopic(d, "events"))));
    if (!d.snapshotUrl.empty())
        v.push_back(entity(d, "sensor", "snapshot_url", "Snapshot URL", "camera/snapshot_url",
                           ",\"entity_category\":\"diagnostic\",\"icon\":\"mdi:image\""));
    return v;
}

}  // namespace aid
