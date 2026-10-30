// Small read-only helpers for system measurements (Linux /proc and /sys).
#pragma once

#include <string>
#include <vector>

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

// A memory card (any /dev/mmcblk* device). `present` = the device node exists, `mounted` = it appears
// in the mount table at `mountPoint`. NOT tested on real hardware (no card was available).
struct SdCard {
    bool present = false, mounted = false;
    std::string device, mountPoint;
};
// Looks for a card; parses `mountsText` (the content of /proc/mounts) so tests can feed a fake one.
SdCard findSdCard(const std::string& mountsText, const std::vector<std::string>& existingDevices);
// Reads the real /proc/mounts and /dev.
SdCard findSdCard();
// Tries to mount an unmounted card read-write at `mountPoint` (vfat, exfat, ext4). True if mounted.
bool tryMountSd(SdCard* card, const std::string& mountPoint, std::string* err);

// First line of a text file without the line ending ("" if unreadable).
std::string firstLine(const std::string& path);
// Value of KEY="value" / KEY=value in a file such as /etc/os-release ("" if absent).
std::string keyValueFromFile(const std::string& path, const std::string& key);

}  // namespace aid
