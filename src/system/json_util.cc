#include "json_util.h"

#include <cstdlib>

namespace aid {

namespace {

bool isWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

void skipWs(const std::string& s, size_t* i) {
    while (*i < s.size() && isWs(s[*i])) ++*i;
}

// Reads a JSON string starting at s[*i] == '"'. Only ASCII \u escapes are supported.
bool readString(const std::string& s, size_t* i, std::string* out) {
    if (*i >= s.size() || s[*i] != '"') return false;
    ++*i;
    out->clear();
    while (*i < s.size() && s[*i] != '"') {
        char c = s[*i];
        if (c == '\\') {
            if (*i + 1 >= s.size()) return false;
            ++*i;
            switch (s[*i]) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'u': {
                    if (*i + 4 >= s.size()) return false;
                    const long v = strtol(s.substr(*i + 1, 4).c_str(), nullptr, 16);
                    if (v < 0x20 || v > 0x7e) return false;
                    c = (char)v;
                    *i += 4;
                    break;
                }
                default: return false;
            }
        } else if ((unsigned char)c < 0x20) {
            return false;
        }
        *out += c;
        ++*i;
    }
    if (*i >= s.size()) return false;  // unterminated
    ++*i;
    return true;
}

}  // namespace

bool jsonGetFlat(const std::string& json, const std::string& key, std::string* out) {
    const std::string pat = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = json.find(pat, pos)) != std::string::npos) {
        size_t i = pos + pat.size();
        skipWs(json, &i);
        if (i >= json.size() || json[i] != ':') { pos = pos + 1; continue; }
        ++i;
        skipWs(json, &i);
        if (i >= json.size()) return false;
        std::string v;
        if (json[i] == '"') {
            ++i;
            while (i < json.size() && json[i] != '"') {
                if (json[i] == '\\' && i + 1 < json.size()) {
                    ++i;
                    switch (json[i]) {
                        case 'n': v += '\n'; break;
                        case 't': v += '\t'; break;
                        case 'r': v += '\r'; break;
                        case 'u': v += '?'; i += 4; break;  // non-ASCII escapes are not supported
                        default: v += json[i];
                    }
                } else {
                    v += json[i];
                }
                ++i;
            }
            if (i >= json.size()) return false;  // unterminated string
        } else {
            while (i < json.size() && json[i] != ',' && json[i] != '}' && !isWs(json[i])) v += json[i++];
        }
        *out = v;
        return true;
    }
    return false;
}

bool jsonParseFlat(const std::string& json, std::map<std::string, std::string>* out) {
    out->clear();
    if (json.size() > 16384) return false;
    size_t i = 0;
    skipWs(json, &i);
    if (i >= json.size() || json[i] != '{') return false;
    ++i;
    skipWs(json, &i);
    if (i < json.size() && json[i] == '}') {
        ++i;
        skipWs(json, &i);
        return i == json.size();
    }
    for (;;) {
        skipWs(json, &i);
        std::string key, val;
        if (!readString(json, &i, &key)) return false;
        skipWs(json, &i);
        if (i >= json.size() || json[i] != ':') return false;
        ++i;
        skipWs(json, &i);
        if (i >= json.size()) return false;
        if (json[i] == '"') {
            if (!readString(json, &i, &val)) return false;
        } else {
            if (json[i] == '{' || json[i] == '[') return false;  // no nesting
            while (i < json.size() && json[i] != ',' && json[i] != '}' && !isWs(json[i])) val += json[i++];
            if (val.empty()) return false;
            for (char c : val) {
                const bool ok = (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || (c >= 'a' && c <= 'z') ||
                                c == 'E';
                if (!ok) return false;
            }
        }
        if (out->size() >= 64) return false;
        (*out)[key] = val;
        skipWs(json, &i);
        if (i >= json.size()) return false;
        if (json[i] == ',') { ++i; continue; }
        if (json[i] == '}') {
            ++i;
            skipWs(json, &i);
            return i == json.size();
        }
        return false;
    }
}

}  // namespace aid
