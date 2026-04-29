// Predator RF — native decoder module registry implementation.
//
// See native_decoder_registry.h for the design contract.

#include "native_decoder_registry.h"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

namespace predator {

namespace {

struct Entry {
    std::string   sourceKey;
    const void*   key;
    NativeDrainFn drain;
};

// The registry lives in sdrpp_core (this TU) so every plugin shares it.
std::mutex&         registryMutex()   { static std::mutex m; return m; }
std::vector<Entry>& registryEntries() { static std::vector<Entry> v; return v; }

} // namespace

void registerNativeDecoder(const void* key,
                           const std::string& sourceKey,
                           NativeDrainFn drain) {
    if (!key || !drain) return;
    std::lock_guard<std::mutex> lk(registryMutex());
    registryEntries().push_back(Entry{sourceKey, key, std::move(drain)});
}

void unregisterNativeDecoder(const void* key) {
    if (!key) return;
    std::lock_guard<std::mutex> lk(registryMutex());
    auto& v = registryEntries();
    v.erase(std::remove_if(v.begin(), v.end(),
                           [key](const Entry& e) { return e.key == key; }),
            v.end());
}

std::vector<NativeDrainBatch> drainAllNativeDecoders(std::size_t maxItemsPerSource) {
    // Snapshot under the lock so a register/unregister on another thread
    // can't invalidate iterators while we're calling user drain code.
    std::vector<Entry> snapshot;
    {
        std::lock_guard<std::mutex> lk(registryMutex());
        snapshot = registryEntries();
    }

    std::vector<NativeDrainBatch> out;
    out.reserve(snapshot.size());
    for (auto& e : snapshot) {
        if (!e.drain) continue;
        auto evs = e.drain(maxItemsPerSource);
        if (evs.empty()) continue;
        out.push_back(NativeDrainBatch{e.sourceKey, std::move(evs)});
    }
    return out;
}

std::size_t nativeDecoderRegistrationCount() {
    std::lock_guard<std::mutex> lk(registryMutex());
    return registryEntries().size();
}

} // namespace predator
