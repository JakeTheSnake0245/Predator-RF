#pragma once

// Predator RF — native decoder module registry.
//
// Native in-APK decoder modules (rtl_433, P25, etc.) are loaded as SDRPP
// plugins, so main_window.cpp cannot reach into them by type. Each native
// decoder module registers a drain callback here when it constructs, and
// unregisters when it destructs. main_window.cpp calls
// `drainAllNativeDecoders(...)` every frame and folds the returned events
// into the same `predatorEvents` stream the bridge ingesters feed.
//
// The registry is process-global (lives in sdrpp_core) so every plugin
// shares the same instance.
//
// Threading: register/unregister happen from the UI thread during module
// construct/destruct. drainAllNativeDecoders is called from the UI
// thread. Drain callbacks themselves must be thread-safe internally
// because the underlying native decoder will populate its queue from
// the DSP thread.

#include "decoder_ingest.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace predator {

// A drain callback returns up to maxItems pending events from a native
// decoder module's internal queue. The registry never stores events
// itself — it just routes the call.
using NativeDrainFn = std::function<std::vector<DecoderIngestEvent>(std::size_t maxItems)>;

// Register a drain callback for a native decoder module instance.
//   key       — opaque per-instance pointer (typically `this`); used by
//               unregisterNativeDecoder() for precise removal at destruct.
//   sourceKey — short family label tagged onto every drained batch,
//               e.g. "RTL433", "P25", "POCSAG".
//   drain     — the per-instance drain function (must be thread-safe).
void registerNativeDecoder(const void* key,
                           const std::string& sourceKey,
                           NativeDrainFn drain);

// Remove every registration whose key matches.
void unregisterNativeDecoder(const void* key);

// Drain up to maxItemsPerSource events from each registered module and
// return the concatenated list, tagged with the sourceKey it came from
// (so the caller knows which decoder family it is — e.g. "RTL433").
struct NativeDrainBatch {
    std::string sourceKey;
    std::vector<DecoderIngestEvent> events;
};
std::vector<NativeDrainBatch> drainAllNativeDecoders(std::size_t maxItemsPerSource = 64);

// Diagnostic: how many native drain callbacks are currently registered.
std::size_t nativeDecoderRegistrationCount();

} // namespace predator
