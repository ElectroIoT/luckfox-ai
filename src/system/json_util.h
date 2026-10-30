// Tiny JSON helpers (we build JSON by hand; no external library on a 128 MB board).
#pragma once

#include <cstdio>
#include <map>
#include <string>

namespace aid {

inline std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else {
                    o += (char)c;
                }
        }
    }
    return o;
}

inline std::string jsonNum(double v, int prec = 2) {
    char b[48];
    snprintf(b, sizeof(b), "%.*f", prec, v);
    return b;
}

// Extracts the value of a top-level string/number/bool member from a flat JSON object such as
// {"a":"x","b":5}. Not a general parser: no nesting, no arrays. Returns false if absent.
bool jsonGetFlat(const std::string& json, const std::string& key, std::string* out);

// Parses a whole flat JSON object into key -> value text (strings unescaped, numbers/true/false/
// null as written). Rejects nested objects/arrays, duplicate-free input is not required (the last
// one wins), more than 64 keys, or input over 16 KB. Returns false on any syntax problem.
bool jsonParseFlat(const std::string& json, std::map<std::string, std::string>* out);

}  // namespace aid
