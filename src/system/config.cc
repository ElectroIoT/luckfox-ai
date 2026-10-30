#include "config.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace aid {

namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

bool toInt(const std::string& v, int* out) {
    if (v.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const long n = strtol(v.c_str(), &end, 10);
    if (errno != 0 || *end != '\0' || n < -1000000 || n > 1000000) return false;
    *out = (int)n;
    return true;
}

bool toFloat(const std::string& v, float* out) {
    if (v.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const double d = strtod(v.c_str(), &end);
    if (errno != 0 || *end != '\0') return false;
    *out = (float)d;
    return true;
}

std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') { out.push_back(trim(cur)); cur.clear(); }
        else cur += c;
    }
    out.push_back(trim(cur));
    return out;
}

bool printable(const std::string& v, size_t maxLen) {
    if (v.size() > maxLen) return false;
    for (char c : v)
        if (c <= ' ' || c > '~') return false;
    return true;
}

bool printableWithSpaces(const std::string& v, size_t maxLen) {
    if (v.size() > maxLen) return false;
    for (char c : v)
        if (c < ' ' || c > '~') return false;
    return true;
}

bool safePath(const std::string& v) {
    if (v.size() < 2 || v.size() > 200 || v[0] != '/') return false;
    if (v.find("..") != std::string::npos) return false;
    for (char c : v)
        if (c <= ' ' || c > '~') return false;
    return true;
}

}  // namespace

bool parseConfig(const std::string& text, Config* out, std::vector<std::string>* problems) {
    std::vector<std::string> local;
    if (!problems) problems = &local;
    bool ok = true;
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    bool zonesReset = false, gpioReset = false;

    auto where = [&]() { return "line " + std::to_string(lineNo) + ": "; };
    auto rangeInt = [&](const std::string& key, const std::string& val, int lo, int hi, int* dst) {
        int n = 0;
        if (!toInt(val, &n)) { problems->push_back(where() + key + " is not a number"); return; }
        if (n < lo || n > hi) {
            problems->push_back(where() + key + " must be " + std::to_string(lo) + ".." + std::to_string(hi));
            ok = false;
            return;
        }
        *dst = n;
    };
    auto boolean = [&](const std::string& key, const std::string& val, bool* dst) {
        if (val == "1" || val == "true" || val == "yes" || val == "on") *dst = true;
        else if (val == "0" || val == "false" || val == "no" || val == "off") *dst = false;
        else problems->push_back(where() + key + " must be 0 or 1");
    };
    auto path = [&](const std::string& key, const std::string& val, std::string* dst) {
        if (!safePath(val)) {
            problems->push_back(where() + key + " must be an absolute path without '..'");
            ok = false;
        } else {
            *dst = val;
        }
    };

    while (std::getline(in, line)) {
        ++lineNo;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        line = trim(line);
        if (line.empty()) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) { problems->push_back(where() + "expected key=value"); continue; }
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));

        if (key == "width") rangeInt(key, val, 32, 2304, &out->width);
        else if (key == "height") rangeInt(key, val, 32, 1296, &out->height);
        else if (key == "fps") rangeInt(key, val, 1, 60, &out->fps);
        else if (key == "bitrate_kbps") rangeInt(key, val, 64, 20000, &out->bitrateKbps);
        else if (key == "gop") rangeInt(key, val, 1, 600, &out->gop);
        else if (key == "jpeg_quality") rangeInt(key, val, 1, 99, &out->jpegQuality);
        else if (key == "brightness") rangeInt(key, val, -1, 255, &out->brightness);
        else if (key == "contrast") rangeInt(key, val, -1, 255, &out->contrast);
        else if (key == "saturation") rangeInt(key, val, -1, 255, &out->saturation);
        else if (key == "sharpness") rangeInt(key, val, -1, 255, &out->sharpness);
        else if (key == "rtsp_port") rangeInt(key, val, 1, 65535, &out->rtspPort);
        else if (key == "http_port") rangeInt(key, val, 1, 65535, &out->httpPort);
        else if (key == "iq_dir") path(key, val, &out->iqDir);
        else if (key == "data_dir") path(key, val, &out->dataDir);
        else if (key == "web_dir") path(key, val, &out->webDir);
        else if (key == "auth_file") path(key, val, &out->authFile);
        else if (key == "model_path") path(key, val, &out->modelPath);
        else if (key == "labels_path") path(key, val, &out->labelsPath);
        else if (key == "rtsp_path") {
            if (val.size() < 2 || val[0] != '/' || val.size() > 64 || val.find("..") != std::string::npos ||
                !printable(val, 64)) {
                problems->push_back(where() + "rtsp_path must look like /live/0");
                ok = false;
            } else {
                out->rtspPath = val;
            }
        } else if (key == "http_token") {
            if (!printable(val, 128)) {
                problems->push_back(where() + "http_token must be printable ASCII without spaces, max 128");
                ok = false;
            } else {
                out->httpToken = val;
            }
        } else if (key == "detect_enabled") boolean(key, val, &out->detectEnabled);
        else if (key == "detect_interval_ms") rangeInt(key, val, 100, 10000, &out->detectIntervalMs);
        else if (key == "detect_conf_pct") rangeInt(key, val, 5, 99, &out->detectConfPct);
        else if (key == "detect_classes") {
            std::vector<std::string> v;
            for (const std::string& c : splitCsv(val)) {
                if (c.empty()) continue;
                if (!printable(c, 32)) { problems->push_back(where() + "bad class name"); ok = false; v.clear(); break; }
                v.push_back(c);
            }
            if (ok) out->detectClasses = v;
        } else if (key == "zone") {
            const std::vector<std::string> p = splitCsv(val);
            float f[4] = {0, 0, 1, 1};
            bool good = p.size() == 4;
            for (size_t i = 0; good && i < 4; ++i) good = toFloat(p[i], &f[i]) && f[i] >= 0.0f && f[i] <= 1.0f;
            if (good && !(f[0] < f[2] && f[1] < f[3])) good = false;
            if (!good) {
                problems->push_back(where() + "zone must be x1,y1,x2,y2 in 0..1 with x1<x2 and y1<y2");
                ok = false;
            } else {
                if (!zonesReset) { out->zones.clear(); zonesReset = true; }
                if (out->zones.size() >= 8) { problems->push_back(where() + "at most 8 zones"); ok = false; }
                else out->zones.push_back(ZoneRect{f[0], f[1], f[2], f[3]});
            }
        } else if (key == "event_cooldown_s") rangeInt(key, val, 0, 86400, &out->eventCooldownS);
        else if (key == "event_absent_s") rangeInt(key, val, 1, 3600, &out->eventAbsentS);
        else if (key == "event_enter_frames") rangeInt(key, val, 1, 20, &out->eventEnterFrames);
        else if (key == "motion_enabled") boolean(key, val, &out->motionEnabled);
        else if (key == "motion_threshold_pct") rangeInt(key, val, 1, 100, &out->motionThresholdPct);
        else if (key == "events_keep") rangeInt(key, val, 10, 2000, &out->eventsKeep);
        else if (key == "snapshots_keep") rangeInt(key, val, 0, 500, &out->snapshotsKeep);
        else if (key == "mqtt_enabled") boolean(key, val, &out->mqttEnabled);
        else if (key == "mqtt_host") {
            if (!printable(val, 128)) { problems->push_back(where() + "mqtt_host invalid"); ok = false; }
            else out->mqttHost = val;
        } else if (key == "mqtt_port") rangeInt(key, val, 1, 65535, &out->mqttPort);
        else if (key == "mqtt_user") {
            if (!printable(val, 64)) { problems->push_back(where() + "mqtt_user invalid"); ok = false; }
            else out->mqttUser = val;
        } else if (key == "mqtt_password") {
            if (!printableWithSpaces(val, 128)) { problems->push_back(where() + "mqtt_password invalid"); ok = false; }
            else out->mqttPassword = val;
        } else if (key == "mqtt_prefix") {
            if (!printable(val, 64) || val.empty() || val[0] == '/' || val[val.size() - 1] == '/' ||
                val.find('#') != std::string::npos || val.find('+') != std::string::npos) {
                problems->push_back(where() + "mqtt_prefix invalid (no leading or trailing /, no #, +)");
                ok = false;
            } else {
                out->mqttPrefix = val;
            }
        } else if (key == "mqtt_device_name") {
            if (!printableWithSpaces(val, 64) || val.empty()) { problems->push_back(where() + "mqtt_device_name invalid"); ok = false; }
            else out->mqttDeviceName = val;
        } else if (key == "mqtt_discovery_prefix") {
            if (!printable(val, 64) || val.empty() || val[0] == '/') { problems->push_back(where() + "mqtt_discovery_prefix invalid"); ok = false; }
            else out->mqttDiscoveryPrefix = val;
        } else if (key == "gpio_enabled") boolean(key, val, &out->gpioEnabled);
        else if (key == "gpio") {
            // gpio=name,role,pin,active_low   (role decides input/output)
            const std::vector<std::string> p = splitCsv(val);
            GpioLine g;
            int pin = 0, al = 0;
            bool good = p.size() == 4 && printable(p[0], 32) && !p[0].empty() && toInt(p[2], &pin) &&
                        pin >= 0 && pin <= 159 && toInt(p[3], &al) && (al == 0 || al == 1);
            const std::string& r = good ? p[1] : std::string();
            if (good && r != "relay" && r != "buzzer" && r != "led" && r != "alarm" && r != "pir" && r != "door") good = false;
            if (!good) {
                problems->push_back(where() + "gpio must be name,role,pin,active_low (role relay|buzzer|led|alarm|pir|door)");
                ok = false;
            } else {
                g.name = p[0];
                g.role = r;
                g.pin = pin;
                g.activeLow = al == 1;
                g.output = !(r == "pir" || r == "door");
                if (!gpioReset) { out->gpio.clear(); gpioReset = true; }
                if (out->gpio.size() >= 16) { problems->push_back(where() + "at most 16 gpio lines"); ok = false; }
                else out->gpio.push_back(g);
            }
        } else if (key == "session_idle_min") rangeInt(key, val, 1, 1440, &out->sessionIdleMin);
        else if (key == "session_max_hours") rangeInt(key, val, 1, 168, &out->sessionMaxHours);
        else problems->push_back(where() + "unknown key '" + key + "'");
    }
    return ok;
}

bool loadConfigFile(const std::string& path, Config* out, std::vector<std::string>* problems,
                    std::string* err) {
    std::ifstream f(path.c_str());
    if (!f) {
        if (err) *err = "cannot read " + path;
        return false;
    }
    std::ostringstream all;
    all << f.rdbuf();
    if (!parseConfig(all.str(), out, problems)) {
        if (err) *err = "invalid value in " + path;
        return false;
    }
    return true;
}

std::string serializeConfig(const Config& c) {
    std::ostringstream o;
    auto b = [](bool v) { return v ? "1" : "0"; };
    char buf[64];
    o << "# aid-core configuration (written by aid-core; comments are not preserved)\n";
    o << "width=" << c.width << "\nheight=" << c.height << "\nfps=" << c.fps << "\n";
    o << "bitrate_kbps=" << c.bitrateKbps << "\ngop=" << c.gop << "\njpeg_quality=" << c.jpegQuality << "\n";
    o << "iq_dir=" << c.iqDir << "\n";
    o << "brightness=" << c.brightness << "\ncontrast=" << c.contrast << "\nsaturation=" << c.saturation
      << "\nsharpness=" << c.sharpness << "\n";
    o << "rtsp_port=" << c.rtspPort << "\nrtsp_path=" << c.rtspPath << "\nhttp_port=" << c.httpPort << "\n";
    o << "http_token=" << c.httpToken << "\n";
    o << "data_dir=" << c.dataDir << "\nweb_dir=" << c.webDir << "\nauth_file=" << c.authFile << "\n";
    o << "detect_enabled=" << b(c.detectEnabled) << "\ndetect_interval_ms=" << c.detectIntervalMs
      << "\ndetect_conf_pct=" << c.detectConfPct << "\nmodel_path=" << c.modelPath
      << "\nlabels_path=" << c.labelsPath << "\n";
    o << "detect_classes=";
    for (size_t i = 0; i < c.detectClasses.size(); ++i) o << (i ? "," : "") << c.detectClasses[i];
    o << "\n";
    for (const ZoneRect& z : c.zones) {
        snprintf(buf, sizeof(buf), "%.4f,%.4f,%.4f,%.4f", z.x1, z.y1, z.x2, z.y2);
        o << "zone=" << buf << "\n";
    }
    o << "event_cooldown_s=" << c.eventCooldownS << "\nevent_absent_s=" << c.eventAbsentS
      << "\nevent_enter_frames=" << c.eventEnterFrames << "\nmotion_enabled=" << b(c.motionEnabled)
      << "\nmotion_threshold_pct=" << c.motionThresholdPct << "\nevents_keep=" << c.eventsKeep
      << "\nsnapshots_keep=" << c.snapshotsKeep << "\n";
    o << "mqtt_enabled=" << b(c.mqttEnabled) << "\nmqtt_host=" << c.mqttHost << "\nmqtt_port=" << c.mqttPort
      << "\nmqtt_user=" << c.mqttUser << "\nmqtt_password=" << c.mqttPassword << "\nmqtt_prefix=" << c.mqttPrefix
      << "\nmqtt_device_name=" << c.mqttDeviceName << "\nmqtt_discovery_prefix=" << c.mqttDiscoveryPrefix << "\n";
    o << "gpio_enabled=" << b(c.gpioEnabled) << "\n";
    for (const GpioLine& g : c.gpio)
        o << "gpio=" << g.name << "," << g.role << "," << g.pin << "," << (g.activeLow ? 1 : 0) << "\n";
    o << "session_idle_min=" << c.sessionIdleMin << "\nsession_max_hours=" << c.sessionMaxHours << "\n";
    return o.str();
}

bool saveConfigFile(const std::string& path, const Config& cfg, std::string* err) {
    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) {
        if (err) *err = "cannot write " + tmp;
        return false;
    }
    chmod(tmp.c_str(), 0600);
    const std::string text = serializeConfig(cfg);
    const bool wrote = fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = fclose(f) == 0;
    if (!wrote || !closed) {
        unlink(tmp.c_str());
        if (err) *err = "write failed for " + tmp;
        return false;
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        if (err) *err = "rename failed for " + path;
        return false;
    }
    return true;
}

}  // namespace aid
