#include "json_util.h"

namespace aid {

bool jsonGetFlat(const std::string& json, const std::string& key, std::string* out) {
    const std::string pat = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = json.find(pat, pos)) != std::string::npos) {
        size_t i = pos + pat.size();
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) ++i;
        if (i >= json.size() || json[i] != ':') { pos = pos + 1; continue; }
        ++i;
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) ++i;
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
            while (i < json.size() && json[i] != ',' && json[i] != '}' && json[i] != ' ' &&
                   json[i] != '\n' && json[i] != '\r' && json[i] != '\t')
                v += json[i++];
        }
        *out = v;
        return true;
    }
    return false;
}

}  // namespace aid
