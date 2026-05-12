#pragma once

// Header-only C++17 port of backend/fusion/tdoa_coordinator.py.
// Controller-mode Predator-RF nodes use this to triangulate emitters
// from peer measurements without a Python backend. Pure stdlib so the
// test runner builds with a single g++ invocation; the iterative LSQ
// solves a 2x2 system per step via Cramer's rule (no LAPACK needed).
//
// Parity contract: outputs MUST match backend/fusion/tdoa_coordinator.py
// for identical inputs. Five points where drift is easy:
//   (1) ENU projection uses ref-node lat for the cosine factor and
//       Earth radius 6_371_000 m (matches Python).
//   (2) range_diffs use SPEED_OF_LIGHT = 299_792_458.0 (exact).
//   (3) iterations capped at 50, eps = 1e-6 added to ranges.
//   (4) ellipse base radius = 50 + (1-conf)*4950, ratio clamped
//       [0.2, 1.0], theta rotated +90 mod 180.
//   (5) 2-node fallback: midpoint, conf=0.3 BEFORE timing scaling.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace predator {
namespace tdoa {

constexpr double kSpeedOfLight = 299792458.0;
constexpr double kEarthRadiusM = 6371000.0;

struct Measurement {
    std::string node_id;
    int64_t timestamp_ns = 0;
    double node_lat = 0.0;
    double node_lon = 0.0;
    double timing_trust = 1.0;  // 0..1, scales final confidence
};

struct Result {
    std::string emitter_id;
    double estimated_lat = 0.0;
    double estimated_lon = 0.0;
    double location_confidence = 0.0;
    std::vector<std::string> participating_nodes;
    std::map<std::string, int64_t> time_differences_ns;  // ordered for parity
    double ellipse_a_m = 0.0;
    double ellipse_b_m = 0.0;
    double ellipse_theta_deg = 0.0;
};

// Caller-side helper: maps a peer's hardware capability flags into the
// timing-trust factor the solver uses. Mirrors the can_do_tdoa /
// timing_stability_trust branches in the Python record_measurement.
inline double computeTimingTrust(bool can_do_tdoa,
                                 double hw_timing_stability) {
    if (can_do_tdoa) {
        return std::max(0.5, std::min(1.0, hw_timing_stability));
    }
    return std::max(0.2, std::min(0.5, hw_timing_stability * 0.5));
}

// Caller-side helper: GPS freshness gate. Returns true when the node's
// last GPS fix is fresh enough to participate in TDOA. gps_updated_ns
// of 0 means "no timestamp supplied" — caller opted out of gating
// (matches Python's bypass-on-zero).
inline bool gpsFresh(int64_t gps_updated_ns, int64_t now_ns,
                     double max_age_s) {
    if (gps_updated_ns <= 0) return true;
    const double age_s = static_cast<double>(now_ns - gps_updated_ns) / 1e9;
    return age_s <= max_age_s;
}

class Coordinator {
public:
    void recordMeasurement(const std::string& emitter_id,
                           const Measurement& m) {
        std::lock_guard<std::mutex> g(mu_);
        pending_[emitter_id].push_back(m);
    }

    void pruneOld(const std::string& emitter_id, double max_age_s,
                  int64_t now_ns) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = pending_.find(emitter_id);
        if (it == pending_.end()) return;
        const int64_t cutoff_ns =
            now_ns - static_cast<int64_t>(max_age_s * 1e9);
        auto& v = it->second;
        v.erase(std::remove_if(v.begin(), v.end(),
                               [cutoff_ns](const Measurement& m) {
                                   return m.timestamp_ns < cutoff_ns;
                               }),
                v.end());
        if (v.empty()) pending_.erase(it);
    }

    int distinctNodes(const std::string& emitter_id) const {
        std::lock_guard<std::mutex> g(mu_);
        auto it = pending_.find(emitter_id);
        if (it == pending_.end()) return 0;
        std::unordered_set<std::string> s;
        for (const auto& m : it->second) s.insert(m.node_id);
        return static_cast<int>(s.size());
    }

    int pendingSize(const std::string& emitter_id) const {
        std::lock_guard<std::mutex> g(mu_);
        auto it = pending_.find(emitter_id);
        return it == pending_.end() ? 0 : static_cast<int>(it->second.size());
    }

    // Atomic pop-and-solve. Returns nullopt if fewer than 2 distinct
    // nodes are pending; in that case the measurements are restored
    // for the next solve attempt (matches Python's re-merge path).
    std::optional<Result> solve(const std::string& emitter_id) {
        std::vector<Measurement> ms;
        {
            std::lock_guard<std::mutex> g(mu_);
            auto it = pending_.find(emitter_id);
            if (it == pending_.end()) return std::nullopt;
            ms = std::move(it->second);
            pending_.erase(it);
        }
        std::unordered_set<std::string> distinct;
        for (const auto& m : ms) distinct.insert(m.node_id);
        if (distinct.size() < 2) {
            std::lock_guard<std::mutex> g(mu_);
            auto& back = pending_[emitter_id];
            for (auto& m : ms) back.push_back(std::move(m));
            return std::nullopt;
        }

        std::sort(ms.begin(), ms.end(),
                  [](const Measurement& a, const Measurement& b) {
                      return a.timestamp_ns < b.timestamp_ns;
                  });
        const Measurement& ref = ms.front();
        std::map<std::string, int64_t> tdiffs;
        for (size_t i = 1; i < ms.size(); ++i) {
            tdiffs[ref.node_id + "->" + ms[i].node_id] =
                ms[i].timestamp_ns - ref.timestamp_ns;
        }

        double lat = 0.0, lon = 0.0, conf = 0.0;
        if (distinct.size() >= 3) {
            triangulate_(ms, lat, lon, conf);
        } else {
            // 2-node fallback: midpoint of one measurement per node.
            std::unordered_set<std::string> seen;
            std::vector<const Measurement*> uniq;
            for (const auto& m : ms) {
                if (seen.insert(m.node_id).second) uniq.push_back(&m);
                if (uniq.size() == 2) break;
            }
            lat = (uniq[0]->node_lat + uniq[1]->node_lat) / 2.0;
            lon = (uniq[0]->node_lon + uniq[1]->node_lon) / 2.0;
            conf = 0.3;
        }

        double ttrust_sum = 0.0;
        for (const auto& m : ms) ttrust_sum += m.timing_trust;
        const double timing_factor = ttrust_sum / static_cast<double>(ms.size());
        conf *= timing_factor;

        double a = 0.0, b = 0.0, theta = 0.0;
        estimateEllipse_(ms, conf, a, b, theta);

        Result r;
        r.emitter_id = emitter_id;
        r.estimated_lat = lat;
        r.estimated_lon = lon;
        r.location_confidence = conf;
        for (const auto& m : ms) r.participating_nodes.push_back(m.node_id);
        r.time_differences_ns = std::move(tdiffs);
        r.ellipse_a_m = a;
        r.ellipse_b_m = b;
        r.ellipse_theta_deg = theta;
        return r;
    }

    // Diagnostics.
    size_t pendingEmitterCount() const {
        std::lock_guard<std::mutex> g(mu_);
        return pending_.size();
    }

    void clear() {
        std::lock_guard<std::mutex> g(mu_);
        pending_.clear();
    }

private:
    static void triangulate_(const std::vector<Measurement>& ms,
                             double& out_lat, double& out_lon,
                             double& out_conf) {
        const Measurement& ref = ms.front();
        const double ref_lat_r = ref.node_lat * M_PI / 180.0;

        std::vector<std::pair<double, double>> pos;
        pos.reserve(ms.size());
        for (const auto& m : ms) {
            const double dlat = (m.node_lat - ref.node_lat) * M_PI / 180.0;
            const double dlon = (m.node_lon - ref.node_lon) * M_PI / 180.0;
            const double e = dlon * std::cos(ref_lat_r) * kEarthRadiusM;
            const double n = dlat * kEarthRadiusM;
            pos.emplace_back(e, n);
        }

        std::vector<double> range_diffs(ms.size(), 0.0);
        for (size_t i = 0; i < ms.size(); ++i) {
            const double dt_s =
                static_cast<double>(ms[i].timestamp_ns - ms[0].timestamp_ns)
                / 1e9;
            range_diffs[i] = dt_s * kSpeedOfLight;
        }

        // Initial estimate: centroid of node positions.
        double ex = 0.0, ey = 0.0;
        for (const auto& p : pos) { ex += p.first; ey += p.second; }
        ex /= static_cast<double>(pos.size());
        ey /= static_cast<double>(pos.size());

        for (int iter = 0; iter < 50; ++iter) {
            const double r0 =
                std::hypot(ex - pos[0].first, ey - pos[0].second) + 1e-6;
            // Build A (n-1 x 2) and b (n-1) implicitly into normal-eq sums.
            // Solve AT*A * delta = AT*b via Cramer (2x2). Equivalent to
            // numpy.linalg.lstsq for full-rank tall A.
            double a00 = 0.0, a01 = 0.0, a11 = 0.0;
            double bx = 0.0, by = 0.0;
            int rows = 0;
            for (size_t i = 1; i < pos.size(); ++i) {
                const double ri =
                    std::hypot(ex - pos[i].first, ey - pos[i].second) + 1e-6;
                const double dx0 = (ex - pos[0].first) / r0;
                const double dy0 = (ey - pos[0].second) / r0;
                const double dxi = (ex - pos[i].first) / ri;
                const double dyi = (ey - pos[i].second) / ri;
                const double rx = dxi - dx0;
                const double ry = dyi - dy0;
                const double rb = range_diffs[i] - (ri - r0);
                a00 += rx * rx;
                a01 += rx * ry;
                a11 += ry * ry;
                bx  += rx * rb;
                by  += ry * rb;
                ++rows;
            }
            if (rows < 2) break;
            // Tikhonov regularizer (lambda=1e-3 of trace) keeps the
            // 2x2 normal-equations solve numerically sane when the
            // node geometry is near-collinear or the time offsets
            // exceed the baseline (rank-deficient direction). Python
            // gets this for free from numpy.linalg.lstsq's SVD; the
            // explicit ridge here is the cheapest stdlib equivalent.
            const double lam = 1e-3 * (a00 + a11);
            const double m00 = a00 + lam;
            const double m11 = a11 + lam;
            const double det = m00 * m11 - a01 * a01;
            if (std::abs(det) < 1e-18) break;
            double dxe = ( m11 * bx - a01 * by) / det;
            double dye = (-a01 * bx + m00 * by) / det;
            // Step cap — even a regularized LSQ can take an ugly first
            // step when residuals are huge; cap to 50 km per iter so
            // a runaway estimate has 50 iterations to recover.
            const double step = std::hypot(dxe, dye);
            if (step > 50000.0) {
                const double s = 50000.0 / step;
                dxe *= s;
                dye *= s;
            }
            ex += dxe;
            ey += dye;
        }

        out_lat = ref.node_lat + (ey / kEarthRadiusM) * 180.0 / M_PI;
        out_lon = ref.node_lon
            + (ex / (kEarthRadiusM * std::cos(ref_lat_r))) * 180.0 / M_PI;
        out_conf = std::min(0.95,
            0.5 + 0.1 * static_cast<double>(ms.size()));
    }

    static void estimateEllipse_(const std::vector<Measurement>& ms,
                                 double conf,
                                 double& out_a, double& out_b,
                                 double& out_theta_deg) {
        const double base = 50.0
            + (1.0 - std::max(0.0, std::min(1.0, conf))) * 4950.0;
        if (ms.size() < 2) {
            out_a = base; out_b = base; out_theta_deg = 0.0;
            return;
        }
        double mlat = 0.0, mlon = 0.0;
        for (const auto& m : ms) { mlat += m.node_lat; mlon += m.node_lon; }
        mlat /= static_cast<double>(ms.size());
        mlon /= static_cast<double>(ms.size());
        const double m_per_deg_lat = 111320.0;
        const double m_per_deg_lon =
            111320.0 * std::max(0.01, std::cos(mlat * M_PI / 180.0));
        std::vector<double> xs, ys;
        xs.reserve(ms.size());
        ys.reserve(ms.size());
        for (const auto& m : ms) {
            xs.push_back((m.node_lon - mlon) * m_per_deg_lon);
            ys.push_back((m.node_lat - mlat) * m_per_deg_lat);
        }
        const double n = static_cast<double>(xs.size());
        double sxx = 0.0, syy = 0.0, sxy = 0.0;
        for (size_t i = 0; i < xs.size(); ++i) {
            sxx += xs[i] * xs[i];
            syy += ys[i] * ys[i];
            sxy += xs[i] * ys[i];
        }
        sxx /= n; syy /= n; sxy /= n;
        const double tr = sxx + syy;
        const double det = sxx * syy - sxy * sxy;
        const double disc = std::max(0.0, (tr / 2.0) * (tr / 2.0) - det);
        const double l1 = tr / 2.0 + std::sqrt(disc);
        const double l2 = std::max(1e-6, tr / 2.0 - std::sqrt(disc));
        double theta = 0.0;
        if (std::abs(sxy) >= 1e-9 || std::abs(sxx - syy) >= 1e-9) {
            theta = 0.5 * std::atan2(2.0 * sxy, sxx - syy) * 180.0 / M_PI;
        }
        const double ratio =
            std::max(0.2, std::min(1.0, std::sqrt(l2 / l1)));
        out_a = base;
        out_b = base * ratio;
        // Rotate +90 mod 180 — TDOA error is across the cluster baseline.
        double t = std::fmod(theta + 90.0, 180.0);
        if (t < 0.0) t += 180.0;
        out_theta_deg = t;
    }

    mutable std::mutex mu_;
    std::map<std::string, std::vector<Measurement>> pending_;
};

}  // namespace tdoa
}  // namespace predator
