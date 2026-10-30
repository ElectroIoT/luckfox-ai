#include "config.h"

#include <cerrno>
#include <cstdlib>
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

}  // namespace

bool parseConfig(const std::string& text, Config* out, std::vector<std::string>* problems) {
    std::vector<std::string> local;
    if (!problems) problems = &local;
    bool ok = true;
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;

    auto rangeInt = [&](const std::string& key, const std::string& val, int lo, int hi, int* dst) {
        int n = 0;
        if (!toInt(val, &n)) {
            problems->push_back("line " + std::to_string(lineNo) + ": " + key + " is not a number");
            return;
        }
        if (n < lo || n > hi) {
            problems->push_back("line " + std::to_string(lineNo) + ": " + key + " must be " +
                                std::to_string(lo) + ".." + std::to_string(hi));
            ok = false;
            return;
        }
        *dst = n;
    };

    while (std::getline(in, line)) {
        ++lineNo;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        line = trim(line);
        if (line.empty()) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            problems->push_back("line " + std::to_string(lineNo) + ": expected key=value");
            continue;
        }
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
        else if (key == "iq_dir") out->iqDir = val;
        else if (key == "rtsp_path") {
            if (val.size() < 2 || val[0] != '/' || val.size() > 64 || val.find("..") != std::string::npos) {
                problems->push_back("line " + std::to_string(lineNo) + ": rtsp_path must look like /live/0");
                ok = false;
            } else {
                out->rtspPath = val;
            }
        } else if (key == "http_token") {
            // Printable ASCII without spaces; length is checked where it is used.
            bool good = val.size() <= 128;
            for (char c : val)
                if (c <= ' ' || c > '~') good = false;
            if (!good) {
                problems->push_back("line " + std::to_string(lineNo) + ": http_token must be printable ASCII, max 128");
                ok = false;
            } else {
                out->httpToken = val;
            }
        } else {
            problems->push_back("line " + std::to_string(lineNo) + ": unknown key '" + key + "'");
        }
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

}  // namespace aid
