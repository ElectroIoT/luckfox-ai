// GPIO via Linux sysfs (Phase 8/9): relay, buzzer, LED, alarm outputs; PIR and door inputs.
//
// DISABLED BY DEFAULT. Which pins are free on a given Luckfox Pico Pro/Max board revision could
// not be verified from here (the pinout is a picture on the vendor wiki and the camera, Ethernet
// and USB use some pins), so this module never chooses a pin: every pin comes from the
// configuration file. Check the pinout and `cat /sys/kernel/debug/gpio` before enabling.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "aid_config.h"

namespace aid {

class Gpio {
public:
    explicit Gpio(const std::string& sysfsRoot = "/sys/class/gpio");
    ~Gpio();

    // Exports and configures every line. Outputs start in their inactive state. On any failure
    // the lines configured so far are released and false is returned.
    bool init(const std::vector<GpioLine>& lines, std::string* err);
    void shutdown();  // outputs -> inactive, lines unexported

    bool setOutput(const std::string& name, bool active, std::string* err);
    bool getState(const std::string& name, bool* active) const;  // outputs: last commanded; inputs: last read

    // Reads inputs with debounce (a level must be seen on two reads 50 ms or more apart) and
    // returns the lines whose state changed. Call about every 100 ms.
    std::vector<std::pair<std::string, bool>> pollInputs(int64_t nowMs);

    std::vector<GpioLine> lines() const { return lines_; }
    bool ready() const { return ready_; }

private:
    struct Runtime {
        bool state = false;       // logical state (after active-low)
        bool candidate = false;
        int64_t candidateSinceMs = 0;
        bool haveCandidate = false;
    };
    bool writeFile(const std::string& path, const std::string& text) const;
    bool readValue(int pin, bool* raw) const;
    void release(const GpioLine& l);

    std::string root_;
    std::vector<GpioLine> lines_;
    std::map<std::string, Runtime> rt_;
    std::vector<int> exported_;   // pins we exported (and must unexport)
    bool ready_ = false;
};

}  // namespace aid
