#pragma once

// CustodyElector — sensor-custody / N-best election for emitter tracks.
//
// C++ port of `backend/coordination/custody_election.py`. Lives here so a
// Predator RF node running in Controller role (KujhadControllerClient peers)
// can run the same election locally without having to round-trip through
// the Python TOC backend. In a deployment with the Python backend present,
// both electors compute the same decision for the same scenario — this is
// guarded by `scripts/test_custody_parity.py` which feeds shared JSON
// fixtures through both implementations and diffs the outputs.
//
// Design contract (mirrors the Python module verbatim):
//
//   * Pure scoring — `scoreNode()` is side-effect-free and depends only on
//     its arguments; the per-track decision cache is a separate concern.
//   * Hard gates first, weighted soft score after — a node missing a
//     required decoder or GPS-sync for a TDOA-required threat scores 0
//     with a `rejected_reason` so the operator can see WHY.
//   * Per-track previous-decision cache keyed by `track_id`.
//   * Handover overlap is expressed in the decision itself
//     (`handover_from`, `tasked_nodes` includes the outgoing primary
//     until `handover_until_ns`), so a tasking layer keeps the old node
//     tuned until the new primary has a clean track.
//   * Two handover branches: (a) NEW handover when primary changes AND
//     old primary is still available; (b) IN-PROGRESS handover inherited
//     from the previous decision while now_ns < handover_until_ns. The
//     deadline is NEVER reset by branch (b) — otherwise a stable primary
//     would keep the outgoing node tasked forever.
//
// This header is intentionally dependency-free (only stdlib + <cmath>) so
// it compiles cleanly on the Android NDK toolchain and in the standalone
// test runner without dragging in nlohmann/json or any networking code.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace predator {
namespace custody {

// ── Tunable defaults — keep in lockstep with Python DEFAULT_* constants ──
inline constexpr int    kDefaultKTotal           = 3;
inline constexpr double kDefaultHandoverOverlapS = 15.0;
inline constexpr double kDefaultStaleGpsAfterS   = 300.0;

inline std::map<std::string, double> defaultWeights() {
    return {
        {"snr",      0.30},
        {"distance", 0.20},
        {"gps_age",  0.10},
        {"trust",    0.20},
        {"load",     0.10},
        {"decoder",  0.10},
    };
}

// ── Inputs ───────────────────────────────────────────────────────────────

struct TrackInput {
    std::string track_id;
    // "low" | "medium" | "high" | "critical" — anything else is treated
    // as low-threat (no GPS-sync hard gate).
    std::string threat_level = "low";
    bool        has_estimated_position = false;
    double      estimated_lat = 0.0;
    double      estimated_lon = 0.0;
    std::string protocol;                          // empty = unknown
    std::vector<std::string> detecting_nodes;       // node_ids that heard it
};

struct NodeInput {
    std::string node_id;
    bool        gps_synchronized = false;
    bool        has_gps_location = false;
    double      gps_lat = 0.0;
    double      gps_lon = 0.0;
    int64_t     gps_updated_ns = 0;                // 0 means never had a fix
    double      sensitivity_trust = 0.5;           // 0.5..1.0 by convention
    // Caller-computed trust score (0.05..0.98 in the Python model). The
    // C++ side doesn't try to recompute it — the Controller has access to
    // peer history via Kujhad and is expected to derive trust there.
    double      trust_score = 0.5;
    std::vector<std::string> available_decoders;   // empty = unknown caps
    bool        thermal_throttling_active = false;
};

// ── Outputs ──────────────────────────────────────────────────────────────

struct Score {
    std::string node_id;
    double      total = 0.0;                       // 0..1
    std::map<std::string, double> components;
    std::string rejected_reason;                   // non-empty iff hard-gated
};

struct Decision {
    std::string track_id;
    int64_t     decided_ns = 0;

    // Tasking
    std::string primary;                           // empty if no eligible node
    std::vector<std::string> backups;
    std::vector<std::string> tasked_nodes;
    std::vector<std::string> stand_down;

    // Handover bookkeeping
    std::string handover_from;                     // empty if no handover
    int64_t     handover_until_ns = 0;

    // Explainability
    std::vector<Score> scores;
    std::string reason;

    bool isHandover() const { return !handover_from.empty(); }
};

// ── Geo helper — local copy so this header has no fusion-layer deps ──────

inline double haversineMetres(double a_lat, double a_lon,
                              double b_lat, double b_lon) {
    constexpr double R = 6'371'000.0;
    constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
    const double a_lat_r = a_lat * kDeg2Rad;
    const double b_lat_r = b_lat * kDeg2Rad;
    const double d_lat   = b_lat_r - a_lat_r;
    const double d_lon   = (b_lon - a_lon) * kDeg2Rad;
    const double s1 = std::sin(d_lat / 2.0);
    const double s2 = std::sin(d_lon / 2.0);
    const double h  = s1 * s1 + std::cos(a_lat_r) * std::cos(b_lat_r) * s2 * s2;
    return 2.0 * R * std::asin(std::sqrt(h));
}

// ── Elector ──────────────────────────────────────────────────────────────

class Elector {
public:
    Elector(int    k_total            = kDefaultKTotal,
            double handover_overlap_s = kDefaultHandoverOverlapS,
            double stale_gps_after_s  = kDefaultStaleGpsAfterS)
        : k_total_(k_total < 1 ? 1 : k_total),
          handover_overlap_s_(handover_overlap_s),
          stale_gps_after_s_(stale_gps_after_s),
          weights_(defaultWeights()) {}

    void setWeights(const std::map<std::string, double>& w) {
        // Merge — unspecified keys keep their default weight.
        for (const auto& kv : w) weights_[kv.first] = kv.second;
    }

    // Fired only when the primary changes (not on re-confirmation). The
    // callback runs synchronously inside elect() — the typical wiring
    // pushes a JSON envelope onto the Kujhad event queue and returns
    // immediately. Exceptions are swallowed so a buggy callback can't
    // take down the elector.
    void setOnChange(std::function<void(const Decision&)> cb) {
        on_change_ = std::move(cb);
    }

    int    kTotal()           const { return k_total_; }
    double handoverOverlapS() const { return handover_overlap_s_; }
    double staleGpsAfterS()   const { return stale_gps_after_s_; }

    // Run one election cycle for `track`. Pass `now_ns = -1` to use the
    // wall clock (steady_clock equivalent of time.time_ns); explicit
    // values are for deterministic tests + parity with the Python harness.
    Decision elect(const TrackInput& track,
                   const std::vector<NodeInput>& nodes,
                   int64_t now_ns = -1,
                   const std::map<std::string, int>& node_loads = {}) {
        if (now_ns < 0) {
            using namespace std::chrono;
            now_ns = duration_cast<nanoseconds>(
                system_clock::now().time_since_epoch()).count();
        }

        // unique_lock (not lock_guard) so we can unlock() before the
        // on_change_ callback fires at the bottom of the function.
        std::unique_lock<std::mutex> lk(mtx_);

        // 1. Score every node, then sort by (-total, node_id). The
        //    deterministic node_id tiebreak matters for both fleet-side
        //    reproducibility and the parity test against Python.
        std::vector<Score> scores;
        scores.reserve(nodes.size());
        for (const auto& node : nodes) {
            int load = 0;
            auto it = node_loads.find(node.node_id);
            if (it != node_loads.end()) load = it->second;
            scores.push_back(scoreNode(track, node, now_ns, load));
        }
        std::sort(scores.begin(), scores.end(),
                  [](const Score& a, const Score& b) {
                      if (a.total != b.total) return a.total > b.total;
                      return a.node_id < b.node_id;
                  });

        std::vector<const Score*> eligible;
        for (const auto& s : scores) {
            if (s.total > 0.0) eligible.push_back(&s);
        }
        std::string primary = eligible.empty() ? std::string()
                                                : eligible.front()->node_id;
        std::vector<std::string> backups;
        for (size_t i = 1; i < eligible.size() && (int)backups.size() < k_total_ - 1; ++i) {
            backups.push_back(eligible[i]->node_id);
        }

        // 2. Handover logic — see the contract notes at the top of the
        //    file. Two cases produce a non-empty handover_from in the
        //    current decision: (a) brand-new this tick, or (b) inherited
        //    from a still-open prior overlap window.
        const Decision* previous = nullptr;
        auto prevIt = last_decisions_.find(track.track_id);
        if (prevIt != last_decisions_.end()) previous = &prevIt->second;

        std::string prev_primary = previous ? previous->primary : std::string();
        std::set<std::string> available_ids;
        for (const auto& n : nodes) available_ids.insert(n.node_id);
        const bool prev_primary_still_available =
            !prev_primary.empty() && available_ids.count(prev_primary) > 0;

        std::string handover_from;
        int64_t handover_until_ns = 0;
        bool new_handover_started = false;
        if (!prev_primary.empty()
                && prev_primary != primary
                && prev_primary_still_available) {
            // Case (a): start a new handover window.
            handover_from = prev_primary;
            handover_until_ns = now_ns +
                static_cast<int64_t>(handover_overlap_s_ * 1e9);
            new_handover_started = true;
        } else if (previous
                && !previous->handover_from.empty()
                && previous->handover_from != primary
                && available_ids.count(previous->handover_from) > 0
                && now_ns < previous->handover_until_ns) {
            // Case (b): inherit the in-progress handover and KEEP the
            // existing deadline (do not reset). When now_ns >= deadline
            // this branch falls through, the old primary drops out of
            // tasked_nodes, and stand_down picks it up below.
            handover_from = previous->handover_from;
            handover_until_ns = previous->handover_until_ns;
        }

        std::vector<std::string> tasked;
        auto pushUnique = [&](const std::string& nid) {
            if (nid.empty()) return;
            if (std::find(tasked.begin(), tasked.end(), nid) != tasked.end()) return;
            tasked.push_back(nid);
        };
        pushUnique(primary);
        for (const auto& b : backups) pushUnique(b);
        pushUnique(handover_from);

        // 3. Stand-down = previous tasked - new tasked, sorted for
        //    determinism (matches Python `sorted(set(...))`).
        std::set<std::string> stand_down_set;
        if (previous) {
            std::set<std::string> tasked_set(tasked.begin(), tasked.end());
            for (const auto& nid : previous->tasked_nodes) {
                if (!tasked_set.count(nid)) stand_down_set.insert(nid);
            }
        }
        std::vector<std::string> stand_down(stand_down_set.begin(),
                                             stand_down_set.end());

        // 4. Trim explainability payload to top-N to keep SSE/JSON
        //    payloads bounded — matches `scores[:max(k_total*2, 5)]`.
        const size_t score_keep = std::max<size_t>(
            static_cast<size_t>(k_total_) * 2, 5);
        if (scores.size() > score_keep) scores.resize(score_keep);

        Decision d;
        d.track_id          = track.track_id;
        d.decided_ns        = now_ns;
        d.primary           = primary;
        d.backups           = backups;
        d.tasked_nodes      = tasked;
        d.stand_down        = stand_down;
        d.handover_from     = handover_from;
        d.handover_until_ns = handover_until_ns;
        d.scores            = std::move(scores);
        d.reason            = buildReason(track, primary, backups,
                                          handover_from, d.scores);

        // 5. Cache + change notification. Compare on the empty-vs-set
        //    semantics Python uses ((prev or "") != (cur or "")).
        //    The callback is captured under the lock then invoked
        //    AFTER unlock so a callback that re-enters elect()/forget()
        //    (e.g. SSE push that triggers a synchronous tasking
        //    re-evaluation) can't deadlock. Same reason we copy the
        //    decision into a local before releasing the lock.
        const bool primary_changed = prev_primary != primary;
        last_decisions_[track.track_id] = d;
        ++elections_total_;
        if (primary.empty()) ++elections_no_eligible_node_;
        if (new_handover_started) ++elections_with_handover_;

        std::function<void(const Decision&)> cb_to_fire;
        if (primary_changed && on_change_) cb_to_fire = on_change_;
        // Copy the decision under the lock, then unlock BEFORE firing
        // the callback. A callback that re-enters elect()/forget() —
        // e.g. an SSE push that triggers a synchronous tasking
        // re-evaluation — would otherwise deadlock on the recursive
        // mutex.acquire(). unique_lock::unlock() is safe to call
        // exactly once; the destructor at scope exit is a no-op
        // when already unlocked.
        Decision d_copy = d;
        lk.unlock();
        if (cb_to_fire) {
            try { cb_to_fire(d_copy); } catch (...) { /* swallow */ }
        }
        return d_copy;
    }

    void forget(const std::string& track_id) {
        std::lock_guard<std::mutex> lk(mtx_);
        last_decisions_.erase(track_id);
    }

    bool lastDecision(const std::string& track_id, Decision& out) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = last_decisions_.find(track_id);
        if (it == last_decisions_.end()) return false;
        out = it->second;
        return true;
    }

    // Stats for /metrics + parity test introspection.
    struct Stats {
        int    k_total;
        double handover_overlap_s;
        std::map<std::string, double> weights;
        uint64_t elections_total;
        uint64_t elections_with_handover;
        uint64_t elections_no_eligible_node;
        size_t   tracks_in_cache;
    };
    Stats stats() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return Stats{
            k_total_, handover_overlap_s_, weights_,
            elections_total_, elections_with_handover_,
            elections_no_eligible_node_,
            last_decisions_.size(),
        };
    }

private:
    int    k_total_;
    double handover_overlap_s_;
    double stale_gps_after_s_;
    std::map<std::string, double> weights_;
    std::function<void(const Decision&)> on_change_;

    mutable std::mutex mtx_;
    std::map<std::string, Decision> last_decisions_;
    uint64_t elections_total_ = 0;
    uint64_t elections_with_handover_ = 0;
    uint64_t elections_no_eligible_node_ = 0;

    // ── Scoring ──────────────────────────────────────────────────────

    Score scoreNode(const TrackInput& t, const NodeInput& n,
                    int64_t now_ns, int load) const {
        Score s;
        s.node_id = n.node_id;

        const std::string gate = hardGate(t, n, now_ns);

        // Always compute soft components for explainability — even on a
        // hard-gated node the operator wants to see what it would have
        // scored otherwise.
        s.components["snr"]      = snrComponent(t, n);
        s.components["distance"] = distanceComponent(t, n);
        s.components["gps_age"]  = gpsAgeComponent(n, now_ns);
        s.components["trust"]    = trustComponent(n);
        s.components["load"]     = loadComponent(load);
        s.components["decoder"]  = decoderComponent(t, n);

        if (!gate.empty()) {
            s.rejected_reason = gate;
            s.total = 0.0;
            return s;
        }

        double weighted = 0.0;
        for (const auto& kv : s.components) {
            auto it = weights_.find(kv.first);
            if (it != weights_.end()) weighted += it->second * kv.second;
        }
        // Multiplicative thermal penalty AFTER the weighted sum so it
        // scales the whole score uniformly.
        if (n.thermal_throttling_active) weighted *= 0.5;

        if (weighted < 0.0) weighted = 0.0;
        if (weighted > 1.0) weighted = 1.0;
        s.total = weighted;
        return s;
    }

    std::string hardGate(const TrackInput& t, const NodeInput& n,
                         int64_t now_ns) const {
        const bool tdoa_required =
            (t.threat_level == "high" || t.threat_level == "critical");

        // 1. TDOA threats need a GPS-synced node. Run BEFORE the stale
        //    gate so an unsynced node short-circuits with the more
        //    actionable reason instead of a misleading "stale" reason.
        if (tdoa_required && !n.gps_synchronized) {
            return "tdoa_threat_requires_gps_sync";
        }
        // 2. Stale GPS for a TDOA-required threat — same reason format
        //    as Python (`gps_fix_stale_<seconds>s`).
        if (tdoa_required && n.gps_updated_ns > 0) {
            const double gps_age_s =
                static_cast<double>(now_ns - n.gps_updated_ns) / 1e9;
            if (gps_age_s > stale_gps_after_s_) {
                std::ostringstream oss;
                oss << "gps_fix_stale_" << static_cast<int64_t>(gps_age_s) << "s";
                return oss.str();
            }
        }
        // 3. Decoder gate — only enforced when both the track has a
        //    known protocol AND the node has reported its capabilities
        //    (empty available_decoders means caps probe hasn't run, so
        //    we can't fairly hard-gate).
        if (!t.protocol.empty() && !n.available_decoders.empty()) {
            std::string wanted = t.protocol;
            std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            bool found = false;
            for (const auto& d : n.available_decoders) {
                std::string dl = d;
                std::transform(dl.begin(), dl.end(), dl.begin(),
                               [](unsigned char c){ return std::tolower(c); });
                if (dl == wanted) { found = true; break; }
            }
            if (!found) return "missing_decoder_" + wanted;
        }
        return "";
    }

    double snrComponent(const TrackInput& t, const NodeInput& n) const {
        const bool heard = std::find(t.detecting_nodes.begin(),
                                      t.detecting_nodes.end(),
                                      n.node_id) != t.detecting_nodes.end();
        const double sens = std::max(0.0, (n.sensitivity_trust - 0.5) * 2.0);
        return heard ? (0.7 + 0.3 * sens) : (0.3 + 0.2 * sens);
    }

    double distanceComponent(const TrackInput& t, const NodeInput& n) const {
        if (!t.has_estimated_position || !n.has_gps_location) return 0.5;
        const double d = haversineMetres(t.estimated_lat, t.estimated_lon,
                                          n.gps_lat, n.gps_lon);
        return std::exp(-d / 14'000.0);
    }

    double gpsAgeComponent(const NodeInput& n, int64_t now_ns) const {
        if (n.gps_updated_ns <= 0) return 0.3;
        const double age_s =
            static_cast<double>(now_ns - n.gps_updated_ns) / 1e9;
        if (age_s <= 0)                       return 1.0;
        if (age_s >= stale_gps_after_s_)      return 0.0;
        return 1.0 - (age_s / stale_gps_after_s_);
    }

    double trustComponent(const NodeInput& n) const {
        // Caller-supplied; we just clamp.
        return std::max(0.0, std::min(1.0, n.trust_score));
    }

    double loadComponent(int load) const {
        if (load < 0) load = 0;
        return 1.0 / (1.0 + static_cast<double>(load));
    }

    double decoderComponent(const TrackInput& t, const NodeInput& n) const {
        if (t.protocol.empty())          return 0.5;
        if (n.available_decoders.empty()) return 0.5;
        std::string wanted = t.protocol;
        std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        for (const auto& d : n.available_decoders) {
            std::string dl = d;
            std::transform(dl.begin(), dl.end(), dl.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            if (dl == wanted) return 1.0;
        }
        return 0.0;
    }

    std::string buildReason(const TrackInput& t,
                            const std::string& primary,
                            const std::vector<std::string>& backups,
                            const std::string& handover_from,
                            const std::vector<Score>& scores) const {
        if (primary.empty()) {
            for (const auto& s : scores) {
                if (!s.rejected_reason.empty()) {
                    std::ostringstream oss;
                    oss << "no eligible node — top candidate " << s.node_id
                        << " rejected: " << s.rejected_reason;
                    return oss.str();
                }
            }
            return "no eligible nodes available";
        }
        std::ostringstream oss;
        oss << "primary=" << primary;
        if (!backups.empty()) {
            oss << " backups=";
            for (size_t i = 0; i < backups.size(); ++i) {
                if (i) oss << ',';
                oss << backups[i];
            }
        }
        if (!handover_from.empty()) oss << " handover_from=" << handover_from;
        oss << " threat=" << t.threat_level;
        return oss.str();
    }
};

}  // namespace custody
}  // namespace predator
