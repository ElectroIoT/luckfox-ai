// Event history (Phase 6). Storage is JSON Lines (one flat JSON object per line) plus JPEG
// snapshots in a directory. SQLite was not used: RAM is ~30 MB, flash is small and wear-limited,
// and the volume is a few hundred events, so a bounded text file is simpler and safer.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "event_engine.h"

namespace aid {

class EventStore {
public:
    // keepEvents: events kept in memory and on disk; keepSnapshots: JPEG files kept (0 = none).
    EventStore(const std::string& dataDir, int keepEvents, int keepSnapshots);

    // Creates <dataDir>/ and <dataDir>/snapshots/ and loads existing events.
    bool init(std::string* err);

    void add(const Event& e);

    // Newest first. type "" = all. sinceMs 0 = no lower bound.
    std::vector<Event> query(size_t limit, const std::string& type, int64_t sinceMs) const;
    size_t count() const;

    // Deletes events older than tsMs (and their snapshots). Returns the number removed.
    size_t deleteBefore(int64_t tsMs);
    size_t clearAll();

    // Stores a JPEG for event `id`; returns the file name ("<id>.jpg") or "" on failure.
    std::string saveSnapshot(uint64_t id, const std::vector<uint8_t>& jpeg);
    // Full path for a snapshot file name, or "" if the name is not of the form <digits>.jpg.
    std::string snapshotPath(const std::string& name) const;

    // Highest event id stored (0 if none): the engine continues after it.
    uint64_t maxId() const;

    // Manual photos ("capture" button, Telegram /photo): <dataDir>/captures/<ms since epoch>.jpg,
    // the newest `keepSnapshots` are kept. saveCapture returns the file name or "".
    std::string saveCapture(int64_t tsMs, const std::vector<uint8_t>& jpeg);
    std::vector<std::string> listCaptures() const;     // newest first
    std::string capturePath(const std::string& name) const;   // "" if the name is invalid

    static std::string toJson(const Event& e);
    static std::string toCsvRow(const Event& e);
    static bool fromJson(const std::string& line, Event* out);

private:
    void rewriteFile();            // caller holds mutex_
    void pruneSnapshots();         // caller holds mutex_
    static void pruneDir(const std::string& dir, int keep);
    void removeSnapshotFile(const std::string& name);

    std::string dir_, file_, snapDir_, capDir_;
    int keepEvents_, keepSnapshots_;
    mutable std::mutex mutex_;
    std::vector<Event> events_;    // oldest first
    int sinceRewrite_ = 0;
};

}  // namespace aid
