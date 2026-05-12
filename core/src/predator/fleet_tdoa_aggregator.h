#pragma once

// Header-only aggregator that turns per-peer Kujhad event drains into
// per-emitter TDOAMeasurement batches and asks the coordinator to solve
// when enough distinct nodes have reported. Keeps JSON parsing in the
// caller (main_window.cpp) so this header stays dependency-free and the
// unit tests build with a single g++ invocation.
//
// Emitter keying: frequency rounded to the nearest `freq_quantum_hz`
// (default 1 kHz). Two peers reporting 433.920 MHz and 433.921 MHz
// land in the same emitter bucket; 433.920 vs 433.940 do not.
//
// Solve trigger: when `distinctNodes(emitter) >= solve_min_distinct`
// AND `now_ns - last_solve_ns(emitter) >= cooldown_ns`. Cooldown
// prevents thrashing when bursty events keep crossing the threshold.

#include "predator/tdoa_coordinator.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace predator {
namespace tdoa {

struct PeerObservation {
    std::string node_id;       // peer identity (hash16 or sourceDevice)
    int64_t timestamp_ns = 0;  // when the peer heard the emitter
    double frequency_hz = 0.0;
    double node_lat = 0.0;
    double node_lon = 0.0;
    double timing_trust = 0.5; // caller pre-computes via computeTimingTrust
    int64_t gps_updated_ns = 0;  // 0 = bypass freshness gate
};

struct AggregatorConfig {
    double freq_quantum_hz = 1000.0;   // 1 kHz emitter buckets
    double measurement_ttl_s = 5.0;    // drop measurements older than this
    int solve_min_distinct = 2;        // 2-node midpoint floor
    double solve_cooldown_s = 2.0;     // per-emitter solve rate limit
    double gps_max_age_s = 60.0;       // forwarded to gpsFresh
};

class FleetTDOAAggregator {
public:
    using OnFixCb = std::function<void(const Result&)>;

    explicit FleetTDOAAggregator(AggregatorConfig cfg = {})
        : cfg_(cfg) {}

    void setConfig(const AggregatorConfig& cfg) {
        std::lock_guard<std::mutex> g(mu_);
        cfg_ = cfg;
    }
    AggregatorConfig config() const {
        std::lock_guard<std::mutex> g(mu_);
        return cfg_;
    }

    void setOnFix(OnFixCb cb) {
        std::lock_guard<std::mutex> g(mu_);
        on_fix_ = std::move(cb);
    }

    // Convert a raw frequency to the emitter key used everywhere.
    // Exposed so callers can attribute fixes back to UI rows.
    static std::string emitterKey(double frequency_hz, double quantum_hz) {
        if (quantum_hz <= 0.0) quantum_hz = 1000.0;
        const long long q = static_cast<long long>(
            std::llround(frequency_hz / quantum_hz));
        return "f:" + std::to_string(q * static_cast<long long>(quantum_hz));
    }

    // Ingest one observation. Returns true when the observation was
    // accepted by the coordinator (false on stale GPS or bad coords).
    bool ingest(const PeerObservation& obs, int64_t now_ns) {
        if (!std::isfinite(obs.frequency_hz) || obs.frequency_hz <= 0.0) {
            return false;
        }
        if (!std::isfinite(obs.node_lat) || !std::isfinite(obs.node_lon)) {
            return false;
        }
        if (obs.node_lat < -90.0 || obs.node_lat > 90.0) return false;
        if (obs.node_lon < -180.0 || obs.node_lon > 180.0) return false;

        AggregatorConfig cfg;
        {
            std::lock_guard<std::mutex> g(mu_);
            cfg = cfg_;
        }
        if (!gpsFresh(obs.gps_updated_ns, now_ns, cfg.gps_max_age_s)) {
            stats_dropped_stale_gps_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const std::string key = emitterKey(obs.frequency_hz, cfg.freq_quantum_hz);
        Measurement m;
        m.node_id = obs.node_id;
        m.timestamp_ns = obs.timestamp_ns;
        m.node_lat = obs.node_lat;
        m.node_lon = obs.node_lon;
        m.timing_trust = obs.timing_trust;
        coord_.recordMeasurement(key, m);
        {
            std::lock_guard<std::mutex> g(mu_);
            known_keys_.insert(key);
        }
        stats_ingested_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Per-tick housekeeping + solve dispatch. Call from the controller
    // event loop after draining peer events. Returns the list of fixes
    // emitted this tick (also delivered via on_fix_ if set).
    std::vector<Result> tick(int64_t now_ns) {
        AggregatorConfig cfg;
        OnFixCb cb;
        {
            std::lock_guard<std::mutex> g(mu_);
            cfg = cfg_;
            cb = on_fix_;
        }

        // Serialize the entire tick body so concurrent ticks can't race
        // on the cooldown gate (check + update must be atomic per
        // emitter; relaxing this lets two ticks both pass the gate and
        // emit duplicate fixes for the same burst). The on_fix callback
        // is invoked with this lock held — callers MUST NOT call back
        // into ingest()/tick() from the callback or they will deadlock.
        // Reentrancy is documented in docs/tdoa_controller.md.
        std::lock_guard<std::mutex> tick_g(tick_mu_);

        std::vector<std::string> keys = pendingKeys_();
        std::vector<Result> fixes;
        const int64_t cooldown_ns =
            static_cast<int64_t>(cfg.solve_cooldown_s * 1e9);
        for (const auto& key : keys) {
            coord_.pruneOld(key, cfg.measurement_ttl_s, now_ns);
            if (coord_.distinctNodes(key) < cfg.solve_min_distinct) continue;
            int64_t last = 0;
            {
                std::lock_guard<std::mutex> g(mu_);
                auto it = last_solve_ns_.find(key);
                if (it != last_solve_ns_.end()) last = it->second;
            }
            if (last > 0 && (now_ns - last) < cooldown_ns) continue;

            auto r = coord_.solve(key);
            if (!r) continue;
            {
                std::lock_guard<std::mutex> g(mu_);
                last_solve_ns_[key] = now_ns;
            }
            stats_fixes_.fetch_add(1, std::memory_order_relaxed);
            fixes.push_back(*r);
            if (cb) {
                try { cb(*r); } catch (...) {}
            }
        }
        return fixes;
    }

    // Diagnostics.
    size_t ingested() const {
        return stats_ingested_.load(std::memory_order_relaxed);
    }
    size_t fixes() const {
        return stats_fixes_.load(std::memory_order_relaxed);
    }
    size_t droppedStaleGps() const {
        return stats_dropped_stale_gps_.load(std::memory_order_relaxed);
    }
    size_t pendingEmitterCount() const { return coord_.pendingEmitterCount(); }

    void clear() {
        coord_.clear();
        std::lock_guard<std::mutex> g(mu_);
        last_solve_ns_.clear();
    }

private:
    std::vector<std::string> pendingKeys_() {
        // Mirror of every emitter key ever ingested; the coordinator
        // keeps its own pending map private so we maintain a side
        // index here. Keys with no live pending or recent solve are
        // pruned by `tick` after a solve attempt returns nullopt.
        std::lock_guard<std::mutex> g(mu_);
        return std::vector<std::string>(known_keys_.begin(), known_keys_.end());
    }

    mutable std::mutex mu_;     // protects cfg_, on_fix_, last_solve_ns_, known_keys_
    std::mutex tick_mu_;        // serializes tick() bodies (see comment above)
    AggregatorConfig cfg_;
    Coordinator coord_;
    OnFixCb on_fix_;
    std::unordered_map<std::string, int64_t> last_solve_ns_;
    std::unordered_set<std::string> known_keys_;
    std::atomic<size_t> stats_ingested_{0};
    std::atomic<size_t> stats_fixes_{0};
    std::atomic<size_t> stats_dropped_stale_gps_{0};
};

}  // namespace tdoa
}  // namespace predator
