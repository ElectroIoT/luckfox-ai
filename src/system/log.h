// Application log: prints to stderr and keeps the last lines in memory for the dashboard.
#pragma once

#include <string>
#include <vector>

namespace aid {

enum class LogLevel { Info, Warn, Error };

void logMsg(LogLevel level, const std::string& msg);
inline void logInfo(const std::string& m) { logMsg(LogLevel::Info, m); }
inline void logWarn(const std::string& m) { logMsg(LogLevel::Warn, m); }
inline void logError(const std::string& m) { logMsg(LogLevel::Error, m); }

// Last `max` lines, oldest first. Lines are "HH:MM:SS L text" (local time, level letter).
std::vector<std::string> recentLogLines(size_t max);

}  // namespace aid
