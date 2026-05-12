#pragma once

// Header-only C++17 port of backend/fusion/stationarity_gate.py.
// Stateless w.r.t. tracks — caller owns history. Mirrors the Python
// gate's defaults, dt-floor bypass, fallback-ellipse path, and ellipse
// axis basis exactly so the two implementations produce the same
// accept/reject and motion-state decisions for identical inputs.
//
// Parity contract (cf. backend/fusion/stationarity_gate.py):
//   - DEFAULT_V_MAX_MPS         = 100.0
//   - DEFAULT_DT_FLOOR_S        = 2.0
//   - DEFAULT_HISTORY_MAX       = 20
//   - DEFAULT_STATIONARY_RATIO  = 1.0
//   - DEFAULT_MOBILE_RATIO      = 3.0
//   - DEFAULT_FALLBACK_ELLIPSE_M = 250.0
//   - dt < dt_floor -> BYPASS the velocity check (do NOT clamp dt and
//     compute velocity anyway). Python: `if dt_s < self.dt_floor_s:`
//     short-circuits with bypass="dt_floor".
//   - classify_motion needs >= 2 points (Python: `if len(history) < 2`).
//   - Ellipse basis: only `ellipse_a_m` (semi-major), with
//     fallback_ellipse_m when no entry has it set.
//   - Hysteresis: borderline returns the prior state IF prior is
//     stationary/mobile, else "unknown".

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace predator {
namespace stationarity {

enum class MotionState : int { Stationary = 0, Mobile = 1, Unknown = 2 };

inline const char* toString(MotionState s) {
    switch (s) {
        case MotionState::Stationary: return "stationary";
        case MotionState::Mobile:     return "mobile";
        case MotionState::Unknown:    return "unknown";
    }
    return "unknown";
}

inline MotionState fromString(const std::string& s) {
    if (s == "stationary") return MotionState::Stationary;
    if (s == "mobile")     return MotionState::Mobile;
    return MotionState::Unknown;
}

struct FixCandidate {
    double lat = 0.0;
    double lon = 0.0;
    int64_t timestamp_ns = 0;
    // Optional: <0 means "unknown", matching Python's `Optional[float]=None`.
    double ellipse_a_m = -1.0;
};

struct HistoryPoint {
    double lat = 0.0;
    double lon = 0.0;
    int64_t timestamp_ns = 0;
    double ellipse_a_m = -1.0;  // <0 = unknown
};

struct Config {
    double v_max_mps = 100.0;
    double dt_floor_s = 2.0;
    size_t history_max = 20;
    double stationary_ratio = 1.0;
    double mobile_ratio = 3.0;
    double fallback_ellipse_m = 250.0;
};

struct Verdict {
    bool accepted = false;
    std::string reason;
    // <0 means "no comparison possible" (matches Python None for the
    // implied_velocity_mps field).
    double implied_velocity_mps = -1.0;
    MotionState motion_state = MotionState::Unknown;
    std::string bypass;  // "" | "new_track" | "dt_floor"
};

namespace detail {

inline double haversineM(double lat1, double lon1, double lat2, double lon2) {
    constexpr double R = 6371000.0;
    const double a_lat_r = lat1 * M_PI / 180.0;
    const double b_lat_r = lat2 * M_PI / 180.0;
    const double d_lat = b_lat_r - a_lat_r;
    const double d_lon = (lon2 - lon1) * M_PI / 180.0;
    const double h =
        std::sin(d_lat / 2.0) * std::sin(d_lat / 2.0) +
        std::cos(a_lat_r) * std::cos(b_lat_r) *
        std::sin(d_lon / 2.0) * std::sin(d_lon / 2.0);
    return 2.0 * R * std::asin(std::sqrt(h));
}

inline bool finiteAndInRange(double lat, double lon) {
    if (!std::isfinite(lat) || !std::isfinite(lon)) return false;
    if (lat < -90.0 || lat > 90.0) return false;
    if (lon < -180.0 || lon > 180.0) return false;
    return true;
}

}  // namespace detail

// classify_motion — must be defined before evaluate() because evaluate
// calls it on every code path (matches Python's behaviour of always
// returning a motion_state with the verdict).
inline MotionState classifyMotion(const std::vector<HistoryPoint>& history,
                                  MotionState prior = MotionState::Unknown,
                                  const Config& cfg = {}) {
    if (history.size() < 2) return MotionState::Unknown;

    // Centroid in lat/lon (planar approximation — fine for <=20 pts).
    double cx = 0.0, cy = 0.0;
    for (const auto& h : history) { cx += h.lat; cy += h.lon; }
    const double n = static_cast<double>(history.size());
    cx /= n; cy /= n;

    double sq = 0.0;
    for (const auto& h : history) {
        const double d = detail::haversineM(cx, cy, h.lat, h.lon);
        sq += d * d;
    }
    const double rms = std::sqrt(sq / n);

    // Average ellipse_a_m across history points that have one set.
    double e_sum = 0.0;
    int e_n = 0;
    for (const auto& h : history) {
        if (h.ellipse_a_m >= 0.0) {
            e_sum += h.ellipse_a_m;
            ++e_n;
        }
    }
    double ellipse_avg = (e_n > 0)
        ? (e_sum / static_cast<double>(e_n))
        : cfg.fallback_ellipse_m;
    if (ellipse_avg <= 0.0) ellipse_avg = cfg.fallback_ellipse_m;

    if (rms <= cfg.stationary_ratio * ellipse_avg) {
        return MotionState::Stationary;
    }
    if (rms > cfg.mobile_ratio * ellipse_avg) {
        return MotionState::Mobile;
    }
    // Borderline — keep prior state if it's stationary/mobile.
    if (prior == MotionState::Stationary || prior == MotionState::Mobile) {
        return prior;
    }
    return MotionState::Unknown;
}

inline Verdict evaluate(const FixCandidate& c,
                        const std::vector<HistoryPoint>& history,
                        MotionState prior_motion_state = MotionState::Unknown,
                        const Config& cfg = {}) {
    Verdict v;
    // Validate coords first — NaN/inf propagate silently otherwise.
    if (!detail::finiteAndInRange(c.lat, c.lon)) {
        v.accepted = false;
        v.reason = "invalid_coordinates";
        v.motion_state = classifyMotion(history, prior_motion_state, cfg);
        return v;
    }
    if (c.timestamp_ns <= 0) {
        v.accepted = false;
        v.reason = "invalid_timestamp";
        v.motion_state = classifyMotion(history, prior_motion_state, cfg);
        return v;
    }

    if (history.empty()) {
        v.accepted = true;
        v.reason = "bypass_new_track";
        v.bypass = "new_track";
        v.motion_state = MotionState::Unknown;
        return v;
    }

    const HistoryPoint& last = history.back();
    const double dist_m =
        detail::haversineM(last.lat, last.lon, c.lat, c.lon);
    const double dt_s_raw =
        static_cast<double>(c.timestamp_ns - last.timestamp_ns) / 1e9;
    const double dt_s = std::max(0.0, dt_s_raw);

    // dt_floor BYPASS — do not compute velocity, do not reject.
    // Classifier sees history + the candidate so the post-acceptance
    // motion_state is returned without a one-step lag.
    if (dt_s < cfg.dt_floor_s) {
        std::vector<HistoryPoint> next = history;
        HistoryPoint hp;
        hp.lat = c.lat; hp.lon = c.lon;
        hp.timestamp_ns = c.timestamp_ns; hp.ellipse_a_m = c.ellipse_a_m;
        next.push_back(hp);
        v.accepted = true;
        v.reason = "bypass_dt_floor";
        v.bypass = "dt_floor";
        v.motion_state = classifyMotion(next, prior_motion_state, cfg);
        return v;
    }

    const double velocity = dist_m / dt_s;
    if (velocity > cfg.v_max_mps) {
        v.accepted = false;
        v.reason = "velocity_exceeds_limit";
        v.implied_velocity_mps = velocity;
        v.motion_state = classifyMotion(history, prior_motion_state, cfg);
        return v;
    }

    std::vector<HistoryPoint> next = history;
    HistoryPoint hp;
    hp.lat = c.lat; hp.lon = c.lon;
    hp.timestamp_ns = c.timestamp_ns; hp.ellipse_a_m = c.ellipse_a_m;
    next.push_back(hp);
    v.accepted = true;
    v.reason = "accepted";
    v.implied_velocity_mps = velocity;
    v.motion_state = classifyMotion(next, prior_motion_state, cfg);
    return v;
}

}  // namespace stationarity
}  // namespace predator
