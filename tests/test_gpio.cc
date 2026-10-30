// PC-side tests for the GPIO module against a fake sysfs directory.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "gpio.h"

static int g_fail = 0, g_run = 0;
#define CHECK(cond)                                                   \
    do {                                                              \
        ++g_run;                                                      \
        if (!(cond)) {                                                \
            ++g_fail;                                                 \
            printf("FAIL line %d: %s\n", __LINE__, #cond);            \
        }                                                             \
    } while (0)

static void put(const std::string& path, const std::string& text) {
    std::ofstream f(path.c_str());
    f << text;
}
static std::string get(const std::string& path) {
    std::ifstream f(path.c_str());
    std::string s;
    std::getline(f, s);
    return s;
}
// A fake pin directory, as the kernel would create it after "export".
static void fakePin(const std::string& root, int pin, const std::string& value) {
    const std::string d = root + "/gpio" + std::to_string(pin);
    mkdir(d.c_str(), 0700);
    put(d + "/direction", "in");
    put(d + "/value", value);
}

static aid::GpioLine line(const char* name, const char* role, int pin, bool output, bool activeLow) {
    aid::GpioLine l;
    l.name = name; l.role = role; l.pin = pin; l.output = output; l.activeLow = activeLow;
    return l;
}

int main() {
    char tmpl[] = "/tmp/aidgpioXXXXXX";
    if (!mkdtemp(tmpl)) return 2;
    const std::string root = tmpl;
    put(root + "/export", "");
    put(root + "/unexport", "");
    fakePin(root, 41, "0");   // relay, active high
    fakePin(root, 42, "0");   // buzzer, active low
    fakePin(root, 50, "1");   // PIR input, active high (currently inactive? value 1 = active)
    fakePin(root, 51, "1");   // door input, active low (1 = inactive)

    std::string err;
    {
        aid::Gpio g(root);
        std::vector<aid::GpioLine> ls = {line("relay1", "relay", 41, true, false),
                                         line("buzzer", "buzzer", 42, true, true),
                                         line("pir", "pir", 50, false, false),
                                         line("door", "door", 51, false, true)};
        CHECK(g.init(ls, &err));
        CHECK(g.ready());
        // outputs start inactive: active-high -> low, active-low -> high
        CHECK(get(root + "/gpio41/direction") == "low");
        CHECK(get(root + "/gpio42/direction") == "high");
        CHECK(get(root + "/gpio50/direction") == "in");

        CHECK(g.setOutput("relay1", true, &err));
        CHECK(get(root + "/gpio41/value") == "1");
        bool st = false;
        CHECK(g.getState("relay1", &st) && st);
        CHECK(g.setOutput("buzzer", true, &err));
        CHECK(get(root + "/gpio42/value") == "0");           // active low: on = level 0
        CHECK(g.setOutput("buzzer", false, &err));
        CHECK(get(root + "/gpio42/value") == "1");
        CHECK(!g.setOutput("pir", true, &err));              // inputs cannot be driven
        CHECK(!g.setOutput("nope", true, &err));

        // inputs: initial logical state read at init (pir=1 -> active, door=1 active-low -> inactive)
        CHECK(g.getState("pir", &st) && st);
        CHECK(g.getState("door", &st) && !st);
        // debounce: a change needs two reads >= 50 ms apart
        put(root + "/gpio50/value", "0");
        CHECK(g.pollInputs(1000).empty());
        CHECK(g.pollInputs(1020).empty());                   // too soon
        std::vector<std::pair<std::string, bool>> ch = g.pollInputs(1060);
        CHECK(ch.size() == 1 && ch[0].first == "pir" && !ch[0].second);
        CHECK(g.pollInputs(1200).empty());                   // no repeat
        // a glitch shorter than the debounce time is ignored
        put(root + "/gpio50/value", "1");
        CHECK(g.pollInputs(2000).empty());
        put(root + "/gpio50/value", "0");
        CHECK(g.pollInputs(2030).empty());
        CHECK(g.getState("pir", &st) && !st);
        // active-low input: 0 means active
        put(root + "/gpio51/value", "0");
        g.pollInputs(3000);
        ch = g.pollInputs(3100);
        CHECK(ch.size() == 1 && ch[0].first == "door" && ch[0].second);

        g.shutdown();
        CHECK(!g.ready());
        CHECK(get(root + "/gpio41/direction") == "low");     // outputs left inactive
        CHECK(get(root + "/gpio42/direction") == "high");
    }
    {   // configuration mistakes and missing pins
        aid::Gpio g(root);
        CHECK(!g.init({line("a", "relay", 41, true, false), line("b", "relay", 41, true, false)}, &err));
        CHECK(err.find("used twice") != std::string::npos);
        CHECK(!g.init({line("a", "relay", 41, true, false), line("a", "led", 42, true, false)}, &err));
        CHECK(!g.init({line("x", "relay", 99, true, false)}, &err));       // pin directory never appears
        CHECK(err.find("cannot export pin 99") != std::string::npos);
        CHECK(!g.ready());
    }
    std::string cmd = std::string("rm -rf ") + tmpl;
    CHECK(system(cmd.c_str()) == 0);
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
