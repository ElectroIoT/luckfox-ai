// Small read-only helpers for system measurements (Linux /proc and /sys).
#pragma once

#include <string>

namespace aid {

// SoC temperature in degrees C from thermal_zone0, or -1000 if unavailable.
double readSocTempC();
// MemAvailable from /proc/meminfo in MB, or -1 if unavailable.
double memAvailableMB();
// MemTotal from /proc/meminfo in MB, or -1 if unavailable.
double memTotalMB();
// Resident memory of this process (VmRSS) in MB, or -1 if unavailable.
double processRssMB();
// Seconds since this function was first called (call it early in main()).
double uptimeSeconds();
// Seconds since boot from /proc/uptime, or -1.
double systemUptimeSeconds();

// First non-loopback IPv4 address as text ("" if none).
std::string localIpv4();
// Total and free space of the filesystem holding `path`, in MB. False if statvfs fails.
bool diskUsageMB(const std::string& path, double* totalMB, double* freeMB);
// First line of a text file without the line ending ("" if unreadable).
std::string firstLine(const std::string& path);
// Value of KEY="value" / KEY=value in a file such as /etc/os-release ("" if absent).
std::string keyValueFromFile(const std::string& path, const std::string& key);

}  // namespace aid
