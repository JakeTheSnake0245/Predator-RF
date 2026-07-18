#pragma once

// EventRingStore — durable file-backed companion to the in-memory Kujhad
// event ring on a Device node.
//
// The in-memory ring (gEventRing in the web backend, `events` in the GUI)
// is what a coordinator replays through `GET /v1/events?since=<serial>`.
// Without persistence, a node reboot or battery death erases every event
// the coordinator hasn't polled yet. This store appends each event to a
// JSONL log as it's pushed, and on restart rehydrates the ring and the
// serial counter so pre-crash events are re-served with their original
// serials and timestamps — serial continuity is preserved (no reuse).
//
// Format: one JSON object per line ("JSONL"). Two segments live in the
// storage directory:
//
//   events.cur.jsonl   — active segment, appended to
//   events.prev.jsonl  — previous segment, read-only
//
// When the active segment reaches `capacity` lines it is rotated onto
// events.prev.jsonl (replacing it) and a fresh active segment starts.
// Disk usage is therefore bounded at ~2x capacity events. On load, prev
// is read before cur (serials are monotonic so this yields oldest-first
// order) and only the newest `capacity` rows are kept, matching the
// in-memory ring bound.
//
// Durability model: each append is fwrite + fflush (buffered to the OS,
// no fsync). This is cheap enough for hit/decode event rates on a
// Raspberry Pi — a hard power cut can lose at most the small tail still
// in the kernel page cache, never the whole ring. Corrupt / truncated
// tail lines from a mid-write crash are skipped at load time.
//
// Thread safety: append() and flush() are mutex-guarded; open() and
// close() must not race append() (call open() before the server starts
// pushing events).
//
// Platform paths: the caller resolves the directory — the daemon config
// root on Linux (`<root>/kujhad_events`), the app files dir on Android.
// The GUI build persists its ring through config.json instead and only
// needs the serial restored, so it does not use this store.

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <sys/stat.h>
#ifdef _WIN32
  #include <direct.h>
#endif

#include "../json.hpp"

namespace predator {

class EventRingStore {
public:
    EventRingStore() = default;
    ~EventRingStore() { close(); }
    EventRingStore(const EventRingStore&) = delete;
    EventRingStore& operator=(const EventRingStore&) = delete;

    // Open (creating the directory if needed) and rehydrate. `serialKey`
    // is the JSON field carrying the monotonic serial ("id" for the web
    // backend ring, "serial" for the GUI ring). Returns false when the
    // directory can't be created or the active segment can't be opened
    // for append — the caller should log and continue memory-only.
    bool open(const std::string& dir, size_t capacity, const std::string& serialKey = "id") {
        std::lock_guard<std::mutex> lk(mtx_);
        closeLocked();
        dir_ = dir;
        capacity_ = capacity ? capacity : 1;
        serialKey_ = serialKey;
        loaded_.clear();
        maxSerial_ = 0;
        curLines_ = 0;
        if (!makeDir(dir_)) return false;

        // Rehydrate: prev first, then cur — serials are monotonic so this
        // reads oldest-first. Bad/truncated lines are skipped.
        readSegment(prevPath());
        bool curEndsClean = true;
        curLines_ = readSegment(curPath(), &curEndsClean);
        if (loaded_.size() > capacity_) {
            loaded_.erase(loaded_.begin(), loaded_.end() - (long)capacity_);
        }

        cur_ = std::fopen(curPath().c_str(), "ab");
        if (!cur_) { loaded_.clear(); return false; }
        // A mid-write crash can leave a truncated tail with no newline.
        // Terminate it so the next append starts on a fresh line instead
        // of concatenating into the garbage.
        if (!curEndsClean) {
            std::fputc('\n', cur_);
            std::fflush(cur_);
        }
        openOk_ = true;
        return true;
    }

    // Events recovered by the last open(), oldest-first, at most
    // `capacity` of them. Valid until the next open()/close().
    const std::vector<nlohmann::json>& loaded() const { return loaded_; }

    // Highest serial seen across both segments at open() time. The
    // caller must restart its serial counter ABOVE this value so
    // post-restart events never reuse a persisted serial.
    uint64_t maxSerial() const { return maxSerial_; }

    // Append one event (already carrying its serial + timestamp).
    // fwrite + fflush, no fsync. Rotates when the active segment hits
    // capacity. No-op when the store failed to open.
    void append(const nlohmann::json& ev) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!openOk_ || !cur_) return;
        if (curLines_ >= capacity_) rotateLocked();
        if (!cur_) return;
        std::string line = ev.dump();
        line.push_back('\n');
        std::fwrite(line.data(), 1, line.size(), cur_);
        std::fflush(cur_);
        curLines_++;
    }

    void close() {
        std::lock_guard<std::mutex> lk(mtx_);
        closeLocked();
    }

    bool isOpen() const { return openOk_; }

private:
    std::string curPath() const { return dir_ + "/events.cur.jsonl"; }
    std::string prevPath() const { return dir_ + "/events.prev.jsonl"; }

    static bool makeDir(const std::string& d) {
        if (d.empty()) return false;
#ifdef _WIN32
        int r = ::_mkdir(d.c_str());
#else
        int r = ::mkdir(d.c_str(), 0700);
#endif
        if (r == 0) return true;
        struct stat st;
        return ::stat(d.c_str(), &st) == 0 && (st.st_mode & S_IFDIR);
    }

    // Read one segment into loaded_, tracking maxSerial_. Returns the
    // number of valid lines read (used to seed curLines_).
    size_t readSegment(const std::string& path, bool* endsClean = nullptr) {
        if (endsClean) *endsClean = true;
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return 0;
        size_t valid = 0;
        std::string line;
        int c;
        while ((c = std::fgetc(f)) != EOF) {
            if (c != '\n') { line.push_back((char)c); continue; }
            if (consumeLine(line)) valid++;
            line.clear();
        }
        // A trailing line without '\n' is a mid-write crash tail — try it
        // anyway; the JSON parse rejects it when truncated.
        if (!line.empty()) {
            if (consumeLine(line)) valid++;
            if (endsClean) *endsClean = false;
        }
        std::fclose(f);
        return valid;
    }

    bool consumeLine(const std::string& line) {
        nlohmann::json ev = nlohmann::json::parse(line, nullptr, false);
        if (ev.is_discarded() || !ev.is_object()) return false;
        uint64_t serial = 0;
        auto it = ev.find(serialKey_);
        if (it != ev.end()) {
            if (it->is_number_unsigned()) serial = it->get<uint64_t>();
            else if (it->is_number_integer()) {
                int64_t v = it->get<int64_t>();
                if (v > 0) serial = (uint64_t)v;
            }
        }
        if (serial > maxSerial_) maxSerial_ = serial;
        loaded_.push_back(std::move(ev));
        return true;
    }

    void rotateLocked() {
        if (cur_) { std::fclose(cur_); cur_ = nullptr; }
        std::remove(prevPath().c_str());
        std::rename(curPath().c_str(), prevPath().c_str());
        cur_ = std::fopen(curPath().c_str(), "wb");
        curLines_ = 0;
        if (!cur_) openOk_ = false;
    }

    void closeLocked() {
        if (cur_) { std::fclose(cur_); cur_ = nullptr; }
        openOk_ = false;
    }

    std::mutex mtx_;
    std::string dir_;
    std::string serialKey_ = "id";
    size_t capacity_ = 1;
    size_t curLines_ = 0;
    std::FILE* cur_ = nullptr;
    bool openOk_ = false;
    std::vector<nlohmann::json> loaded_;
    uint64_t maxSerial_ = 0;
};

} // namespace predator
