// Predator RF — process-global Kraken tune bus implementation.
//
// See kraken_tune_bus.h for the design contract.

#include "kraken_tune_bus.h"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

namespace predator {

namespace {

struct Entry {
    const void*    key;
    KrakenTuneFn   tune;
    KrakenStatusFn status;
};

std::mutex&         busMutex()   { static std::mutex m; return m; }
std::vector<Entry>& busEntries() { static std::vector<Entry> v; return v; }

// Snapshot the most recent registration under the lock, then call the
// handler outside the lock so a slow handler can't stall register paths.
bool latestEntry(Entry& out) {
    std::lock_guard<std::mutex> lk(busMutex());
    if (busEntries().empty()) return false;
    out = busEntries().back();
    return true;
}

} // namespace

void registerKrakenTuner(const void* key, KrakenTuneFn tune, KrakenStatusFn status) {
    if (!key || !tune || !status) return;
    std::lock_guard<std::mutex> lk(busMutex());
    busEntries().push_back(Entry{key, std::move(tune), std::move(status)});
}

void unregisterKrakenTuner(const void* key) {
    if (!key) return;
    std::lock_guard<std::mutex> lk(busMutex());
    auto& v = busEntries();
    v.erase(std::remove_if(v.begin(), v.end(),
                           [key](const Entry& e) { return e.key == key; }),
            v.end());
}

bool krakenTunerAvailable() {
    std::lock_guard<std::mutex> lk(busMutex());
    return !busEntries().empty();
}

bool requestKrakenTune(double freqHz) {
    if (freqHz <= 0.0) return false;
    Entry e;
    if (!latestEntry(e)) return false;
    return e.tune ? e.tune(freqHz) : false;
}

KrakenTuneSnapshot krakenTuneSnapshot() {
    Entry e;
    if (!latestEntry(e) || !e.status) return KrakenTuneSnapshot{};
    KrakenTuneSnapshot snap = e.status();
    snap.available = true;
    return snap;
}

} // namespace predator
