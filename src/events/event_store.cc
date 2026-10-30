#include "event_store.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "json_util.h"

namespace aid {

namespace {

bool mkdirP(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!cur.empty() && cur != "/") {
                if (mkdir(cur.c_str(), 0700) != 0 && access(cur.c_str(), F_OK) != 0) return false;
            }
        }
        if (i < path.size()) cur += path[i];
    }
    return true;
}

bool validSnapshotName(const std::string& n) {
    if (n.size() < 5 || n.size() > 24) return false;
    if (n.compare(n.size() - 4, 4, ".jpg") != 0) return false;
    for (size_t i = 0; i + 4 < n.size(); ++i)
        if (n[i] < '0' || n[i] > '9') return false;
    return true;
}

std::string csvField(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"') o += "\"\"";
        else if (c == '\n' || c == '\r') o += ' ';
        else o += c;
    }
    return o + "\"";
}

}  // namespace

EventStore::EventStore(const std::string& dataDir, int keepEvents, int keepSnapshots)
    : dir_(dataDir), file_(dataDir + "/events.jsonl"), snapDir_(dataDir + "/snapshots"),
      capDir_(dataDir + "/captures"), keepEvents_(keepEvents), keepSnapshots_(keepSnapshots) {}

std::string EventStore::toJson(const Event& e) {
    std::string s = "{\"id\":" + std::to_string(e.id) + ",\"ts\":" + std::to_string((long long)e.tsMs) +
                    ",\"type\":\"" + jsonEscape(e.type) + "\",\"label\":\"" + jsonEscape(e.label) +
                    "\",\"conf\":" + jsonNum(e.confidence, 3) + ",\"count\":" + std::to_string(e.count) +
                    ",\"snapshot\":\"" + jsonEscape(e.snapshot) + "\",\"detail\":\"" + jsonEscape(e.detail) + "\"}";
    return s;
}

std::string EventStore::toCsvRow(const Event& e) {
    return std::to_string(e.id) + "," + std::to_string((long long)e.tsMs) + "," + csvField(e.type) + "," +
           csvField(e.label) + "," + jsonNum(e.confidence, 3) + "," + std::to_string(e.count) + "," +
           csvField(e.snapshot) + "," + csvField(e.detail);
}

bool EventStore::fromJson(const std::string& line, Event* out) {
    std::string v;
    Event e;
    if (!jsonGetFlat(line, "id", &v)) return false;
    e.id = strtoull(v.c_str(), nullptr, 10);
    if (e.id == 0) return false;
    if (!jsonGetFlat(line, "ts", &v)) return false;
    e.tsMs = strtoll(v.c_str(), nullptr, 10);
    if (!jsonGetFlat(line, "type", &e.type) || e.type.empty()) return false;
    jsonGetFlat(line, "label", &e.label);
    if (jsonGetFlat(line, "conf", &v)) e.confidence = (float)atof(v.c_str());
    if (jsonGetFlat(line, "count", &v)) e.count = atoi(v.c_str());
    jsonGetFlat(line, "snapshot", &e.snapshot);
    jsonGetFlat(line, "detail", &e.detail);
    if (!e.snapshot.empty() && !validSnapshotName(e.snapshot)) e.snapshot.clear();
    *out = e;
    return true;
}

bool EventStore::init(std::string* err) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mkdirP(snapDir_) || !mkdirP(capDir_)) {
        if (err) *err = "cannot create " + snapDir_;
        return false;
    }
    events_.clear();
    std::ifstream f(file_.c_str());
    std::string line;
    while (std::getline(f, line)) {
        Event e;
        if (fromJson(line, &e)) events_.push_back(e);
    }
    while ((int)events_.size() > keepEvents_) events_.erase(events_.begin());
    return true;
}

void EventStore::add(const Event& e) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(e);
    FILE* f = fopen(file_.c_str(), "a");
    if (f) {
        const std::string line = toJson(e) + "\n";
        fwrite(line.data(), 1, line.size(), f);
        fclose(f);
    }
    bool trimmed = false;
    while ((int)events_.size() > keepEvents_) {
        const std::string snap = events_.front().snapshot;
        events_.erase(events_.begin());
        if (!snap.empty()) removeSnapshotFile(snap);
        trimmed = true;
    }
    if (trimmed && ++sinceRewrite_ >= 50) rewriteFile();
}

void EventStore::rewriteFile() {
    const std::string tmp = file_ + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return;
    for (const Event& e : events_) {
        const std::string line = toJson(e) + "\n";
        fwrite(line.data(), 1, line.size(), f);
    }
    const bool ok = fclose(f) == 0;
    if (ok) rename(tmp.c_str(), file_.c_str());
    else unlink(tmp.c_str());
    sinceRewrite_ = 0;
}

std::vector<Event> EventStore::query(size_t limit, const std::string& type, int64_t sinceMs) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Event> out;
    for (auto it = events_.rbegin(); it != events_.rend() && out.size() < limit; ++it) {
        if (!type.empty() && it->type != type) continue;
        if (sinceMs > 0 && it->tsMs < sinceMs) continue;
        out.push_back(*it);
    }
    return out;
}

size_t EventStore::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_.size();
}

void EventStore::removeSnapshotFile(const std::string& name) {
    if (!validSnapshotName(name)) return;
    unlink((snapDir_ + "/" + name).c_str());
}

size_t EventStore::deleteBefore(int64_t tsMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t removed = 0;
    std::vector<Event> keep;
    for (const Event& e : events_) {
        if (e.tsMs < tsMs) {
            if (!e.snapshot.empty()) removeSnapshotFile(e.snapshot);
            ++removed;
        } else {
            keep.push_back(e);
        }
    }
    events_.swap(keep);
    if (removed) rewriteFile();
    return removed;
}

size_t EventStore::clearAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t n = events_.size();
    for (const Event& e : events_)
        if (!e.snapshot.empty()) removeSnapshotFile(e.snapshot);
    events_.clear();
    rewriteFile();
    return n;
}

void EventStore::pruneSnapshots() { pruneDir(snapDir_, keepSnapshots_); }

void EventStore::pruneDir(const std::string& dir, int keep) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::pair<unsigned long long, std::string>> files;
    while (struct dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (validSnapshotName(n)) files.push_back({strtoull(n.c_str(), nullptr, 10), n});
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    size_t drop = files.size() > (size_t)keep ? files.size() - (size_t)keep : 0;
    for (size_t i = 0; i < drop; ++i) unlink((dir + "/" + files[i].second).c_str());
}

uint64_t EventStore::maxId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t m = 0;
    for (const Event& e : events_) m = std::max<uint64_t>(m, e.id);
    return m;
}

std::string EventStore::saveCapture(int64_t tsMs, const std::vector<uint8_t>& jpeg) {
    if (jpeg.empty() || tsMs <= 0) return "";
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string name = std::to_string((long long)tsMs) + ".jpg";
    const std::string path = capDir_ + "/" + name;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return "";
    const bool ok = fwrite(jpeg.data(), 1, jpeg.size(), f) == jpeg.size();
    fclose(f);
    if (!ok) { unlink(path.c_str()); return ""; }
    pruneDir(capDir_, std::max(keepSnapshots_, 1));
    return access(path.c_str(), F_OK) == 0 ? name : "";
}

std::vector<std::string> EventStore::listCaptures() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<unsigned long long, std::string>> files;
    if (DIR* d = opendir(capDir_.c_str())) {
        while (struct dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (validSnapshotName(n)) files.push_back({strtoull(n.c_str(), nullptr, 10), n});
        }
        closedir(d);
    }
    std::sort(files.rbegin(), files.rend());
    std::vector<std::string> out;
    for (const auto& f : files) out.push_back(f.second);
    return out;
}

std::string EventStore::capturePath(const std::string& name) const {
    if (!validSnapshotName(name)) return "";
    return capDir_ + "/" + name;
}

std::string EventStore::saveSnapshot(uint64_t id, const std::vector<uint8_t>& jpeg) {
    if (keepSnapshots_ <= 0 || jpeg.empty()) return "";
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string name = std::to_string((unsigned long long)id) + ".jpg";
    const std::string path = snapDir_ + "/" + name;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return "";
    const bool ok = fwrite(jpeg.data(), 1, jpeg.size(), f) == jpeg.size();
    fclose(f);
    if (!ok) { unlink(path.c_str()); return ""; }
    pruneSnapshots();
    // pruning may have removed this very file if keepSnapshots_ is tiny; report what exists
    return access(path.c_str(), F_OK) == 0 ? name : "";
}

std::string EventStore::snapshotPath(const std::string& name) const {
    if (!validSnapshotName(name)) return "";
    return snapDir_ + "/" + name;
}

}  // namespace aid
