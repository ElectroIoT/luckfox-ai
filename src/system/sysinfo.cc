#include "sysinfo.h"

#include <time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace aid {

double readSocTempC() {
    FILE* f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!f) return -1000;
    long v = 0;
    const int n = fscanf(f, "%ld", &v);
    fclose(f);
    return n == 1 ? v / 1000.0 : -1000;
}

static double kbFromFile(const char* path, const char* key) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char line[160];
    const size_t klen = strlen(key);
    double result = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) == 0) {
            result = atof(line + klen) / 1024.0;
            break;
        }
    }
    fclose(f);
    return result;
}

double memAvailableMB() { return kbFromFile("/proc/meminfo", "MemAvailable:"); }
double processRssMB() { return kbFromFile("/proc/self/status", "VmRSS:"); }

double uptimeSeconds() {
    static struct timespec start = {0, 0};
    static bool init = false;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!init) {
        start = now;
        init = true;
    }
    return (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
}

}  // namespace aid
