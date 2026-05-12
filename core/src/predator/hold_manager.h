// SPDX-License-Identifier: GPL-3.0-only
//
// predator::hold::HoldManager — multi-VFO hold list.
//
// Why this exists
// ---------------
// Predator-RF already implements an in-bandwidth "hold" pattern via the
// per-hit marker VFOs in main_window.cpp (see the per-frame re-anchor
// loop near `// Every frame: re-anchor all Predator marker VFOs ...`).
// That works as long as:
//   (a) the held frequency stays inside the SDR's instantaneous
//       bandwidth, AND
//   (b) the underlying hit row is still in `hits[]` (hits get aged out
//       and pruned on a regular cadence, taking the marker with them).
//
// Multi-VFO Hold (roadmap item #4) lifts both restrictions:
//   1. A held entry persists across hit pruning AND across app restart
//      via core::configManager.
//   2. When the source retunes such that a held frequency falls
//      OUTSIDE the current wideband window, the corresponding VFO is
//      torn down cleanly (so we don't leave a dead channel sitting at
//      the spectrum edge), and re-created when it comes back in band.
//   3. Each held entry carries its own decoder selection (NBFM, WBFM,
//      RTL433, ADS-B, DSDFME P25, ...) so the operator can pin "P25
//      control + ADS-B 1090 + ISM 433" and have each one decoded by
//      the right native module rather than the default Radio module.
//
// What this header is NOT
// -----------------------
// * NOT a round-robin source-retune scheduler. If two held entries are
//   on different bands (e.g. 433 MHz + 1090 MHz) and the SDR can only
//   see one at a time, this manager will simply mark the out-of-band
//   one inactive — it will not retune the source automatically. That's
//   a separate roadmap feature ("Multi-band scheduler") that wraps
//   THIS manager and adds a dwell-weighted retune loop.
//
// Test surface
// ------------
// Header-only, pure stdlib (+ the project's bundled nlohmann/json for
// persistence). VFO operations are injected via std::function so the
// standalone test runner (tests/hold_manager_test.cpp) can mock them
// without linking against sigpath / ImGui.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "../json.hpp"

namespace predator {
namespace hold {

// Decoder selections the operator can pin to a held frequency.
// String mapping is the persistence wire format — keep names stable.
enum class DecoderKind {
    Radio_NBFM,
    Radio_WBFM,
    Radio_AM,
    Radio_USB,
    Radio_LSB,
    Radio_DSB,
    Radio_RAW,
    Native_RTL433,
    Native_ADSB,
    Native_DSDFME_P25,
};

inline const char* decoderKindToString(DecoderKind k) {
    switch (k) {
        case DecoderKind::Radio_NBFM:        return "radio_nbfm";
        case DecoderKind::Radio_WBFM:        return "radio_wbfm";
        case DecoderKind::Radio_AM:          return "radio_am";
        case DecoderKind::Radio_USB:         return "radio_usb";
        case DecoderKind::Radio_LSB:         return "radio_lsb";
        case DecoderKind::Radio_DSB:         return "radio_dsb";
        case DecoderKind::Radio_RAW:         return "radio_raw";
        case DecoderKind::Native_RTL433:     return "native_rtl433";
        case DecoderKind::Native_ADSB:       return "native_adsb";
        case DecoderKind::Native_DSDFME_P25: return "native_dsdfme_p25";
    }
    return "radio_nbfm";  // unreachable; satisfies -Wreturn-type
}

inline bool decoderKindFromString(const std::string& s, DecoderKind& out) {
    if (s == "radio_nbfm")        { out = DecoderKind::Radio_NBFM;        return true; }
    if (s == "radio_wbfm")        { out = DecoderKind::Radio_WBFM;        return true; }
    if (s == "radio_am")          { out = DecoderKind::Radio_AM;          return true; }
    if (s == "radio_usb")         { out = DecoderKind::Radio_USB;         return true; }
    if (s == "radio_lsb")         { out = DecoderKind::Radio_LSB;         return true; }
    if (s == "radio_dsb")         { out = DecoderKind::Radio_DSB;         return true; }
    if (s == "radio_raw")         { out = DecoderKind::Radio_RAW;         return true; }
    if (s == "native_rtl433")     { out = DecoderKind::Native_RTL433;     return true; }
    if (s == "native_adsb")       { out = DecoderKind::Native_ADSB;       return true; }
    if (s == "native_dsdfme_p25") { out = DecoderKind::Native_DSDFME_P25; return true; }
    return false;
}

// What the operator pinned. Persisted across restarts.
struct HoldEntry {
    std::string id;                // stable, opaque (e.g. "h7"); used as VFO suffix
    std::string label;             // operator-visible name; auto-derived from freq if empty
    double frequency_hz   = 0.0;   // absolute centre frequency to hold
    double bandwidth_hz   = 12500.0;
    DecoderKind decoder   = DecoderKind::Radio_NBFM;
    int64_t created_ns    = 0;     // wall clock at add(); for sort stability
    bool enabled          = true;  // operator can pause without removing
};

// Per-entry runtime state. NOT persisted — derived from tick().
struct HoldEntryRuntime {
    bool in_band                   = false;
    bool vfo_active                = false;
    int64_t last_status_change_ns  = 0;
    std::string vfo_name;          // populated only while vfo_active==true
};

struct TickStats {
    int created   = 0;
    int destroyed = 0;
    int anchored  = 0;
};

class HoldManager {
public:
    // Callbacks injected by the wire-up so the manager itself stays
    // free of sigpath / ImGui dependencies (= testable from g++).
    //
    // CreateFn returns true on success. If it returns false, the
    // entry's runtime stays inactive and the next tick will retry
    // (handles transient sigpath::vfoManager.createVFO() failures
    // when the source is mid-retune).
    using CreateFn  = std::function<bool(const HoldEntry& e,
                                         const std::string& vfoName,
                                         double initialOffsetHz)>;
    using DestroyFn = std::function<void(const std::string& vfoName)>;
    using AnchorFn  = std::function<void(const std::string& vfoName,
                                         double offsetHz,
                                         double bwHz)>;
    // ExistsFn lets tick() reconcile rt.vfo_active against external
    // reality. If a decoder module reload, manual operator action, or
    // any other path outside the HoldManager destroys "Predator H<id>"
    // out from under us, internal state would otherwise stay stuck at
    // vfo_active=true forever (never retrying createCb). When supplied
    // and it returns false, the manager forgets the VFO and re-creates
    // on the same tick. Optional — null callback preserves the older
    // behaviour for callers that own the VFO lifecycle exclusively.
    using ExistsFn  = std::function<bool(const std::string& vfoName)>;

    HoldManager() = default;

    // Configuration knobs.
    size_t maxEntries() const            { return max_entries_; }
    void   setMaxEntries(size_t n)       { max_entries_ = n; }
    double duplicateToleranceHz() const  { return dup_tol_hz_; }
    void   setDuplicateToleranceHz(double v) { dup_tol_hz_ = v; }

    // ---- Mutators ---------------------------------------------------

    // Returns the new entry id on success, empty string on rejection
    // (duplicate within dup_tol_hz_, max_entries_ reached, or invalid
    // frequency / bandwidth). Caller can re-call list() to learn why
    // by inspecting size() vs maxEntries() and existing frequencies.
    std::string add(double freqHz, double bwHz, DecoderKind decoder,
                    const std::string& label, int64_t now_ns) {
        if (!std::isfinite(freqHz) || freqHz <= 0.0)            return "";
        if (!std::isfinite(bwHz)   || bwHz   <= 0.0)            return "";
        if (entries_.size() >= max_entries_)                    return "";
        for (const auto& e : entries_) {
            if (std::fabs(e.frequency_hz - freqHz) <= dup_tol_hz_) return "";
        }
        HoldEntry e;
        e.id           = nextId();
        e.label        = label;
        e.frequency_hz = freqHz;
        e.bandwidth_hz = bwHz;
        e.decoder      = decoder;
        e.created_ns   = now_ns;
        e.enabled      = true;
        entries_.push_back(e);
        // Runtime starts inactive — next tick will create the VFO if
        // the entry is currently in-band.
        runtime_[e.id] = HoldEntryRuntime{};
        return e.id;
    }

    bool remove(const std::string& id) {
        auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&](const HoldEntry& e){ return e.id == id; });
        if (it == entries_.end()) return false;
        entries_.erase(it);
        // Runtime entry is left in place so the next tick's GC pass
        // sees vfo_active==true and fires the destroy callback. We
        // don't have the callback handy at remove() time — by design,
        // remove() is a pure-data op, side effects ride the next tick.
        return true;
    }

    void setEnabled(const std::string& id, bool enabled) {
        for (auto& e : entries_) {
            if (e.id == id) { e.enabled = enabled; return; }
        }
    }

    void clear() {
        entries_.clear();
        // Runtime kept until next tick GCs and tears down VFOs.
    }

    // ---- Read accessors --------------------------------------------

    size_t size() const { return entries_.size(); }
    const std::vector<HoldEntry>& entries() const { return entries_; }

    // Returns a default-constructed runtime for unknown ids so the UI
    // can render rows without exception handling.
    HoldEntryRuntime runtimeFor(const std::string& id) const {
        auto it = runtime_.find(id);
        return (it == runtime_.end()) ? HoldEntryRuntime{} : it->second;
    }

    // ---- Lifecycle drive -------------------------------------------

    // Call once per UI frame from the wire-up. createCb / destroyCb /
    // anchorCb are invoked synchronously; tick() does no work between
    // callbacks so it's safe for them to grab sigpath mutexes.
    TickStats tick(double sourceCenterHz, double sampleRateHz, int64_t now_ns,
                   const CreateFn& createCb,
                   const DestroyFn& destroyCb,
                   const AnchorFn& anchorCb,
                   const ExistsFn& existsCb = ExistsFn{}) {
        TickStats s{};
        // 1) For every live entry: reconcile in-band + vfo_active.
        for (auto& e : entries_) {
            auto& rt = runtime_[e.id];  // creates default-init if absent
            // Reality-check: if an external code path destroyed our
            // VFO, drop the stale "active" flag so the create branch
            // below can rebuild it. Don't fire destroyCb here — the
            // VFO is already gone.
            if (rt.vfo_active && existsCb && !existsCb(rt.vfo_name)) {
                rt.vfo_active = false;
                rt.vfo_name.clear();
                rt.last_status_change_ns = now_ns;
            }
            const bool inBandNow  = inBand(e.frequency_hz, e.bandwidth_hz,
                                           sourceCenterHz, sampleRateHz);
            const bool wantActive = e.enabled && inBandNow;
            if (inBandNow != rt.in_band) {
                rt.in_band = inBandNow;
                rt.last_status_change_ns = now_ns;
            }
            if (wantActive && !rt.vfo_active) {
                const std::string vname  = makeVfoName(e.id);
                const double initialOff  = e.frequency_hz - sourceCenterHz;
                if (createCb && createCb(e, vname, initialOff)) {
                    rt.vfo_active = true;
                    rt.vfo_name   = vname;
                    rt.last_status_change_ns = now_ns;
                    s.created++;
                }
                // If createCb returned false (or is null), leave runtime
                // inactive and try again next tick.
            } else if (!wantActive && rt.vfo_active) {
                if (destroyCb) destroyCb(rt.vfo_name);
                rt.vfo_active = false;
                rt.vfo_name.clear();
                rt.last_status_change_ns = now_ns;
                s.destroyed++;
            }
            if (wantActive && rt.vfo_active && anchorCb) {
                anchorCb(rt.vfo_name,
                         e.frequency_hz - sourceCenterHz,
                         e.bandwidth_hz);
                s.anchored++;
            }
        }
        // 2) GC: runtime entries whose parent HoldEntry was removed
        //    must have their VFOs torn down.
        for (auto it = runtime_.begin(); it != runtime_.end(); ) {
            bool stillLive = false;
            for (const auto& e : entries_) {
                if (e.id == it->first) { stillLive = true; break; }
            }
            if (!stillLive) {
                if (it->second.vfo_active && destroyCb) {
                    destroyCb(it->second.vfo_name);
                    s.destroyed++;
                }
                it = runtime_.erase(it);
            } else {
                ++it;
            }
        }
        return s;
    }

    // ---- Persistence ------------------------------------------------

    nlohmann::json toJson() const {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : entries_) {
            nlohmann::json row;
            row["id"]            = e.id;
            row["label"]         = e.label;
            row["frequency_hz"]  = e.frequency_hz;
            row["bandwidth_hz"]  = e.bandwidth_hz;
            row["decoder"]       = decoderKindToString(e.decoder);
            row["created_ns"]    = e.created_ns;
            row["enabled"]       = e.enabled;
            arr.push_back(row);
        }
        return arr;
    }

    // Replaces the entry list. Runtime is reset — on next tick all
    // currently-in-band entries will be (re)created with fresh VFOs.
    // Caller is responsible for running ONE tick with destroyCb wired
    // BEFORE calling fromJson() if they need to tear down VFOs from
    // the previous entry set.
    void fromJson(const nlohmann::json& j) {
        entries_.clear();
        runtime_.clear();
        id_counter_ = 0;
        if (!j.is_array()) return;
        for (const auto& row : j) {
            if (!row.is_object()) continue;
            HoldEntry e;
            e.id           = row.value("id", std::string{});
            e.label        = row.value("label", std::string{});
            e.frequency_hz = row.value("frequency_hz", 0.0);
            e.bandwidth_hz = row.value("bandwidth_hz", 12500.0);
            e.created_ns   = row.value("created_ns", (int64_t)0);
            e.enabled      = row.value("enabled", true);
            std::string dec = row.value("decoder", std::string("radio_nbfm"));
            decoderKindFromString(dec, e.decoder);
            // Skip rows that would be invalid via add()'s rules so
            // that bad config data can't permanently wedge us.
            if (!std::isfinite(e.frequency_hz) || e.frequency_hz <= 0.0) continue;
            if (!std::isfinite(e.bandwidth_hz) || e.bandwidth_hz <= 0.0) continue;
            if (entries_.size() >= max_entries_) break;
            // Auto-mint an id if the persisted one is missing/blank
            // (forward-compat with hand-edited config.json).
            if (e.id.empty()) e.id = nextId();
            // Bump id_counter_ above any persisted "h<n>" so future
            // adds don't collide.
            if (e.id.size() > 1 && e.id[0] == 'h') {
                try {
                    uint64_t n = (uint64_t)std::stoull(e.id.substr(1));
                    if (n + 1 > id_counter_) id_counter_ = n + 1;
                } catch (...) {}
            }
            entries_.push_back(e);
            runtime_[e.id] = HoldEntryRuntime{};
        }
    }

    // ---- Pure helpers (also useful to tests) -----------------------

    // True when the entry's filter passband fits ENTIRELY inside the
    // SDR's instantaneous bandwidth. Half-bandwidth guard on each side
    // prevents the channel from being clipped at the spectrum edge.
    static bool inBand(double freq, double bw, double center, double sr) {
        if (!std::isfinite(freq) || !std::isfinite(bw)) return false;
        if (!std::isfinite(center) || !std::isfinite(sr) || sr <= 0.0) return false;
        const double half_sr = sr * 0.5;
        const double half_bw = bw * 0.5;
        return (freq + half_bw) <= (center + half_sr) &&
               (freq - half_bw) >= (center - half_sr);
    }

    // VFO names use a "Predator H<id>" convention so they sort next to
    // the existing "Predator M<n>" marker VFOs in the waterfall list.
    static std::string makeVfoName(const std::string& id) {
        return std::string("Predator H") + id;
    }

private:
    std::string nextId() {
        return std::string("h") + std::to_string(++id_counter_);
    }

    std::vector<HoldEntry> entries_;
    std::unordered_map<std::string, HoldEntryRuntime> runtime_;
    size_t   max_entries_ = 8;
    double   dup_tol_hz_  = 100.0;
    uint64_t id_counter_  = 0;
};

}  // namespace hold
}  // namespace predator
