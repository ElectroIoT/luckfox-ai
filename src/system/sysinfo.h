// Small read-only helpers for system measurements (Linux /proc and /sys).
#pragma once

namespace aid {

// SoC temperature in degrees C from thermal_zone0, or -1000 if unavailable.
double readSocTempC();
// MemAvailable from /proc/meminfo in MB, or -1 if unavailable.
double memAvailableMB();
// Resident memory of this process (VmRSS) in MB, or -1 if unavailable.
double processRssMB();
// Seconds since this function was first called (call it early in main()).
double uptimeSeconds();

}  // namespace aid
