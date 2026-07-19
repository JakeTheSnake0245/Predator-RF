#pragma once

// Predator RF — process-global Kraken tune bus.
//
// The KrakenSDR control client (kraken_ctl_client.h) lives inside the
// kraken_lob_decoder plugin instance, so main_window.cpp and the Android
// map bridge cannot reach it by type. Mirroring native_decoder_registry,
// the kraken module registers a tune handler + status snapshot provider
// here on construct and unregisters on destruct. Any UI surface (Hits
// list "task Kraken" button, map emitter-marker tap routed through the
// Android WebView bridge) then just calls requestKrakenTune(freqHz) and
// polls krakenTuneSnapshot() for lifecycle feedback
// (sending → calibrating → confirmed / failed).
//
// The registry lives in sdrpp_core (kraken_tune_bus.cpp) so every plugin
// shares the same instance.
//
// Threading: register/unregister happen on the UI thread during module
// construct/destruct. requestKrakenTune / krakenTuneSnapshot are called
// from the UI thread (main_window per-frame tick). Handlers themselves
// must be cheap and non-blocking — the real tune runs on the control
// client's worker thread.
//
// RX-only posture: this bus only carries receive-retune requests for a
// passive DF array. No transmit capability is exposed or implied.

#include <functional>
#include <string>

namespace predator {

// Lifecycle snapshot for UI feedback. `state` mirrors
// KrakenTuneState (kraken_ctl_client.h): 0 idle, 1 sending,
// 2 calibrating, 3 confirmed, 4 failed.
struct KrakenTuneSnapshot {
    bool        available   = false;  // a tuner is registered
    bool        running     = false;  // control client worker is on
    bool        reachable   = false;  // last settings GET succeeded
    int         state       = 0;      // KrakenTuneState as int
    std::string status;               // human-readable status line
    double      currentHz   = 0.0;    // last readback centre freq
    double      requestedHz = 0.0;    // last requested tune freq
};

// Handler signature: attempt a retune to freqHz. Returns false when the
// request could not even be queued (another tune in flight, bad freq).
using KrakenTuneFn   = std::function<bool(double freqHz)>;
// Snapshot provider: fill everything except `available`.
using KrakenStatusFn = std::function<KrakenTuneSnapshot()>;

// Register the (single logical) Kraken tuner. If several kraken module
// instances register, the most recent registration wins.
//   key — opaque per-instance pointer (typically `this`).
void registerKrakenTuner(const void* key, KrakenTuneFn tune, KrakenStatusFn status);

// Remove the registration whose key matches.
void unregisterKrakenTuner(const void* key);

// True when a tuner is registered (drives button visibility).
bool krakenTunerAvailable();

// Ask the registered tuner to retune. Returns false when no tuner is
// registered or the handler rejected the request.
bool requestKrakenTune(double freqHz);

// Current lifecycle snapshot. `available=false` when nothing registered.
KrakenTuneSnapshot krakenTuneSnapshot();

} // namespace predator
