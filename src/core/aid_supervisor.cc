// aid-supervisor: keeps aid-core running, as a small compiled program that never touches the flash
// in steady state (no sleep/date/find processes to start, code pages locked in RAM).
//   - waits for the stock camera app (rkipc), stops it and the USB udhcpc that inherits its socket
//   - starts aid-core, restarts it if it exits (growing pause), kills it if its heartbeat goes stale
//   - exit code 3 from aid-core = "restart requested" (not counted as a failure)
//   - after 5 quick failures in a row it gives up and starts the stock app again
//   - if the root filesystem turns read-only (the SPI flash controller problem seen in testing,
//     see docs/streaming.md) and the board has been up for 5 minutes, it syncs and reboots the board
//     with the reboot system call, which needs nothing from the broken flash
// Stop it with: touch /tmp/aid-stop (or SIGTERM).  Usage:
//   aid-supervisor [--core /root/aid/aid-core] [--config /root/aid/aid.conf] [--log /tmp/aid-core.log]
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifndef MCL_ONFAULT
#define MCL_ONFAULT 4
#endif

namespace {

volatile sig_atomic_t g_term = 0;
void onTerm(int) { g_term = 1; }

const char* kStopFile = "/tmp/aid-stop";
const char* kHeartbeat = "/tmp/aid-core.heartbeat";
const char* kPidFile = "/tmp/aid-supervisor.pid";
int g_logFd = -1;

long monoS() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec;
}
long wallS() { return (long)time(nullptr); }

void logLine(const std::string& msg) {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char head[40];
    snprintf(head, sizeof(head), "%02d:%02d:%02d supervisor: ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    const std::string line = std::string(head) + msg + "\n";
    if (g_logFd >= 0) {
        if (write(g_logFd, line.data(), line.size()) < 0) { /* nothing more we can do */ }
    }
}

bool exists(const char* p) { return access(p, F_OK) == 0; }

double systemUptime() {
    FILE* f = fopen("/proc/uptime", "r");
    if (!f) return 0;
    double up = 0;
    if (fscanf(f, "%lf", &up) != 1) up = 0;
    fclose(f);
    return up;
}

// True if the filesystem mounted at "/" is mounted read-only (reads /proc/mounts: no flash I/O).
bool rootReadOnly() {
    FILE* f = fopen("/proc/mounts", "r");
    if (!f) return false;
    char line[512];
    bool ro = false;
    while (fgets(line, sizeof(line), f)) {
        char dev[128], mnt[128], type[64], opts[256];
        if (sscanf(line, "%127s %127s %63s %255s", dev, mnt, type, opts) != 4) continue;
        if (strcmp(mnt, "/") != 0) continue;
        ro = false;
        std::string o = std::string(",") + opts + ",";
        if (o.find(",ro,") != std::string::npos) ro = true;
    }
    fclose(f);
    return ro;
}

std::vector<int> pidsByName(const char* name) {
    std::vector<int> pids;
    DIR* d = opendir("/proc");
    if (!d) return pids;
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char path[300], comm[64] = {0};
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof(comm), f)) {
            comm[strcspn(comm, "\n")] = 0;
            if (strcmp(comm, name) == 0) pids.push_back(atoi(e->d_name));
        }
        fclose(f);
    }
    closedir(d);
    return pids;
}

void sleepMs(int ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

bool stopRequested() { return g_term || exists(kStopFile); }

void stopStockApp() {
    int waited = 0;
    while (pidsByName("rkipc").empty() && waited < 60 && !stopRequested()) { sleepMs(1000); ++waited; }
    for (int pid : pidsByName("rkipc")) kill(pid, SIGTERM);
    for (int i = 0; i < 20 && !pidsByName("rkipc").empty(); ++i) sleepMs(1000);
    for (int pid : pidsByName("rkipc")) kill(pid, SIGKILL);
    // rkipc's RTSP listening socket is inherited by the USB-network DHCP client: free it
    for (int pid : pidsByName("udhcpc")) kill(pid, SIGTERM);
    sleepMs(2000);
    logLine("stock camera app stopped");
}

}  // namespace

int main(int argc, char** argv) {
    std::string core = "/root/aid/aid-core", conf = "/root/aid/aid.conf", logPath = "/tmp/aid-core.log";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--core")) core = argv[i + 1];
        else if (!strcmp(argv[i], "--config")) conf = argv[i + 1];
        else if (!strcmp(argv[i], "--log")) logPath = argv[i + 1];
        else { fprintf(stderr, "usage: %s [--core f] [--config f] [--log f]\n", argv[0]); return 2; }
    }
    g_logFd = open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    signal(SIGTERM, onTerm);
    signal(SIGINT, onTerm);
    signal(SIGPIPE, SIG_IGN);

    FILE* pf = fopen(kPidFile, "w");
    if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }
    unlink(kStopFile);

    stopStockApp();
    // Keep this small program's pages in RAM from now on (lock what is resident, and later faults).
    if (mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT) != 0) logLine("mlockall not available; continuing without");

    const int kFast = 60, kMaxFastFails = 5;
    int fails = 0;
    while (!stopRequested()) {
        unlink(kHeartbeat);
        const long t0 = monoS();
        const pid_t pid = fork();
        if (pid < 0) { logLine("fork failed"); sleepMs(5000); continue; }
        if (pid == 0) {
            if (g_logFd >= 0) { dup2(g_logFd, 1); dup2(g_logFd, 2); }
            execl(core.c_str(), core.c_str(), "--config", conf.c_str(), (char*)nullptr);
            _exit(127);
        }
        logLine("started aid-core (pid " + std::to_string((int)pid) + ")");

        int status = 0;
        bool exited = false;
        int stale = 0, tick = 0;
        while (!exited) {
            sleepMs(1000);
            ++tick;
            if (waitpid(pid, &status, WNOHANG) == pid) { exited = true; break; }
            if (stopRequested()) {
                kill(pid, SIGTERM);
                for (int i = 0; i < 20 && !exited; ++i) {
                    if (waitpid(pid, &status, WNOHANG) == pid) exited = true;
                    else sleepMs(1000);
                }
                if (!exited) { kill(pid, SIGKILL); waitpid(pid, &status, 0); exited = true; }
                break;
            }
            if (tick % 5 != 0) continue;

            struct stat st;
            if (monoS() - t0 > 90) {
                if (stat(kHeartbeat, &st) != 0 || wallS() - (long)st.st_mtime > 60) ++stale; else stale = 0;
                if (stale >= 3) {
                    logLine("heartbeat is stale: killing aid-core");
                    kill(pid, SIGKILL);
                    waitpid(pid, &status, 0);
                    exited = true;
                    break;
                }
            }
            if (g_logFd >= 0 && fstat(g_logFd, &st) == 0 && st.st_size > 1000000) {
                if (ftruncate(g_logFd, 0) == 0) logLine("log was truncated (over 1 MB)");
            }
            if (rootReadOnly() && systemUptime() > 300) {
                logLine("the root filesystem is read-only (flash errors): syncing and rebooting the board");
                sync();
                reboot(RB_AUTOBOOT);
            }
        }
        const int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        const long ran = monoS() - t0;
        logLine("aid-core exited with code " + std::to_string(rc) + " after " + std::to_string(ran) + "s");
        if (stopRequested()) break;
        if (rc == 3) { fails = 0; sleepMs(2000); continue; }
        fails = ran < kFast ? fails + 1 : 0;
        if (fails >= kMaxFastFails) {
            logLine("aid-core keeps failing: giving up and starting the stock camera app");
            if (system("sh /oem/usr/bin/RkLunch.sh >> /tmp/aid-core.log 2>&1 &") != 0) logLine("could not start the stock app");
            break;
        }
        sleepMs((fails * 5 + 3) * 1000);
    }
    logLine("supervisor stopped");
    unlink(kPidFile);
    return 0;
}
