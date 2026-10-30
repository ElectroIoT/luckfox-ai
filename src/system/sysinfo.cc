#include "sysinfo.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/statvfs.h>
#include <time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

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
double memTotalMB() { return kbFromFile("/proc/meminfo", "MemTotal:"); }
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

double systemUptimeSeconds() {
    FILE* f = fopen("/proc/uptime", "r");
    if (!f) return -1;
    double up = -1;
    if (fscanf(f, "%lf", &up) != 1) up = -1;
    fclose(f);
    return up;
}

std::string localIpv4() {
    struct ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return "";
    std::string result;
    for (struct ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        if (!(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
        char buf[INET_ADDRSTRLEN];
        const struct sockaddr_in* sin = (const struct sockaddr_in*)a->ifa_addr;
        if (!inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) continue;
        if (strncmp(a->ifa_name, "eth", 3) == 0) { result = buf; break; }   // prefer the Ethernet port
        if (result.empty() && strncmp(a->ifa_name, "usb", 3) != 0) result = buf;
    }
    freeifaddrs(list);
    return result;
}

bool diskUsageMB(const std::string& path, double* totalMB, double* freeMB) {
    struct statvfs s;
    if (statvfs(path.c_str(), &s) != 0) return false;
    *totalMB = (double)s.f_blocks * s.f_frsize / (1024.0 * 1024.0);
    *freeMB = (double)s.f_bavail * s.f_frsize / (1024.0 * 1024.0);
    return true;
}

std::string firstLine(const std::string& path) {
    std::ifstream f(path.c_str());
    std::string s;
    if (f) std::getline(f, s);
    // /proc/device-tree/model ends with a NUL byte; also drop the line ending
    while (!s.empty() && (s[s.size() - 1] == '\r' || s[s.size() - 1] == '\n' || s[s.size() - 1] == '\0')) s.erase(s.size() - 1);
    return s;
}

std::string keyValueFromFile(const std::string& path, const std::string& key) {
    std::ifstream f(path.c_str());
    std::string line;
    const std::string prefix = key + "=";
    while (std::getline(f, line)) {
        if (line.compare(0, prefix.size(), prefix) != 0) continue;
        std::string v = line.substr(prefix.size());
        if (v.size() >= 2 && v[0] == '"' && v[v.size() - 1] == '"') v = v.substr(1, v.size() - 2);
        return v;
    }
    return "";
}

}  // namespace aid
