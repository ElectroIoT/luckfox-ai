#include "log.h"

#include <time.h>

#include <cstdio>
#include <deque>
#include <mutex>

namespace aid {

namespace {
std::mutex g_mutex;
std::deque<std::string> g_lines;
constexpr size_t kKeep = 300;
}  // namespace

void logMsg(LogLevel level, const std::string& msg) {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char head[24];
    snprintf(head, sizeof(head), "%02d:%02d:%02d %c ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
             level == LogLevel::Info ? 'I' : (level == LogLevel::Warn ? 'W' : 'E'));
    std::string line = std::string(head) + msg;
    if (line.size() > 400) line.resize(400);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_lines.push_back(line);
        while (g_lines.size() > kKeep) g_lines.pop_front();
    }
    fprintf(stderr, "%s\n", line.c_str());
}

std::vector<std::string> recentLogLines(size_t max) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const size_t n = g_lines.size() < max ? g_lines.size() : max;
    return std::vector<std::string>(g_lines.end() - (long)n, g_lines.end());
}

}  // namespace aid
