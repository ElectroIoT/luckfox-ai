// Runtime configuration for aid-core, read from a simple key=value file.
// No secrets live in the repository: the HTTP token is only set in the file on the board.
#pragma once

#include <string>
#include <vector>

namespace aid {

struct Config {
    // video
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
    // network
    int rtspPort = 554;
    std::string rtspPath = "/live/0";
    int httpPort = 8080;
    // HTTP access token. Empty = the HTTP server listens on 127.0.0.1 only.
    std::string httpToken;
};

// Parses text in `key=value` lines ('#' starts a comment). Unknown keys and bad values are
// reported in *problems; a bad value keeps the default. Returns false only on a hard error
// (a value out of its allowed range).
bool parseConfig(const std::string& text, Config* out, std::vector<std::string>* problems);

// Reads and parses a file. Returns false (with a message in *err) if the file cannot be read
// or a value is out of range.
bool loadConfigFile(const std::string& path, Config* out, std::vector<std::string>* problems,
                    std::string* err);

}  // namespace aid
