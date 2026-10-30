#include "gpio.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>

#include "log.h"

namespace aid {

Gpio::Gpio(const std::string& sysfsRoot) : root_(sysfsRoot) {}
Gpio::~Gpio() { shutdown(); }

bool Gpio::writeFile(const std::string& path, const std::string& text) const {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    return fclose(f) == 0 && ok;
}

bool Gpio::readValue(int pin, bool* raw) const {
    FILE* f = fopen((root_ + "/gpio" + std::to_string(pin) + "/value").c_str(), "r");
    if (!f) return false;
    char c = 0;
    const bool ok = fread(&c, 1, 1, f) == 1;
    fclose(f);
    if (!ok || (c != '0' && c != '1')) return false;
    *raw = c == '1';
    return true;
}

void Gpio::release(const GpioLine& l) {
    if (l.output) {
        const std::string dir = root_ + "/gpio" + std::to_string(l.pin);
        writeFile(dir + "/direction", l.activeLow ? "high" : "low");   // inactive level, no glitch
    }
}

bool Gpio::init(const std::vector<GpioLine>& lines, std::string* err) {
    shutdown();
    std::string dummy;
    if (!err) err = &dummy;
    // The same pin or name twice is a configuration mistake.
    for (size_t i = 0; i < lines.size(); ++i)
        for (size_t j = i + 1; j < lines.size(); ++j) {
            if (lines[i].pin == lines[j].pin) { *err = "pin " + std::to_string(lines[i].pin) + " is used twice"; return false; }
            if (lines[i].name == lines[j].name) { *err = "name '" + lines[i].name + "' is used twice"; return false; }
        }
    for (const GpioLine& l : lines) {
        const std::string dir = root_ + "/gpio" + std::to_string(l.pin);
        if (access(dir.c_str(), F_OK) != 0) {
            if (!writeFile(root_ + "/export", std::to_string(l.pin)) || access(dir.c_str(), F_OK) != 0) {
                *err = "cannot export pin " + std::to_string(l.pin) + " (" + l.name + "): busy, not a GPIO, or no permission";
                shutdown();
                return false;
            }
            exported_.push_back(l.pin);
        }
        const bool ok = l.output ? writeFile(dir + "/direction", l.activeLow ? "high" : "low")
                                 : writeFile(dir + "/direction", "in");
        if (!ok) {
            *err = "cannot set direction of pin " + std::to_string(l.pin) + " (" + l.name + ")";
            lines_.push_back(l);   // so shutdown() releases it
            shutdown();
            return false;
        }
        lines_.push_back(l);
        Runtime r;
        if (!l.output) {
            bool raw = false;
            if (readValue(l.pin, &raw)) r.state = l.activeLow ? !raw : raw;
        }
        rt_[l.name] = r;
    }
    ready_ = true;
    return true;
}

void Gpio::shutdown() {
    for (const GpioLine& l : lines_) release(l);
    for (int pin : exported_) writeFile(root_ + "/unexport", std::to_string(pin));
    exported_.clear();
    lines_.clear();
    rt_.clear();
    ready_ = false;
}

bool Gpio::setOutput(const std::string& name, bool active, std::string* err) {
    std::string dummy;
    if (!err) err = &dummy;
    for (const GpioLine& l : lines_) {
        if (l.name != name) continue;
        if (!l.output) { *err = name + " is an input"; return false; }
        const bool level = l.activeLow ? !active : active;
        if (!writeFile(root_ + "/gpio" + std::to_string(l.pin) + "/value", level ? "1" : "0")) {
            *err = "write failed for " + name;
            return false;
        }
        rt_[name].state = active;
        return true;
    }
    *err = "unknown gpio '" + name + "'";
    return false;
}

bool Gpio::getState(const std::string& name, bool* active) const {
    auto it = rt_.find(name);
    if (it == rt_.end()) return false;
    *active = it->second.state;
    return true;
}

std::vector<std::pair<std::string, bool>> Gpio::pollInputs(int64_t nowMs) {
    std::vector<std::pair<std::string, bool>> changes;
    for (const GpioLine& l : lines_) {
        if (l.output) continue;
        bool raw = false;
        if (!readValue(l.pin, &raw)) continue;
        const bool logical = l.activeLow ? !raw : raw;
        Runtime& r = rt_[l.name];
        if (logical == r.state) { r.haveCandidate = false; continue; }
        if (!r.haveCandidate || r.candidate != logical) {
            r.candidate = logical;
            r.candidateSinceMs = nowMs;
            r.haveCandidate = true;
        } else if (nowMs - r.candidateSinceMs >= 50) {
            r.state = logical;
            r.haveCandidate = false;
            changes.push_back({l.name, logical});
        }
    }
    return changes;
}

}  // namespace aid
