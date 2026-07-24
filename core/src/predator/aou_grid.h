#pragma once
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <cstdint>

/*
 * Grid-based transmitter Area-Of-Uncertainty (AOU) estimator.
 *
 * Concept (KrakenSDR "mobile vehicular operation" approach): instead of
 * intersecting bearing lines, overlay a geographic grid on the operating
 * area and let every observation "paint" likelihood onto the cells it is
 * consistent with. Over time / observers the paint converges: the hot
 * region is the AOU, its peak the best transmitter estimate.
 *
 * Observation types:
 *  - Bearing observations (KrakenSDR DoA): a Gaussian fan around the
 *    reported bearing, sigma = reported bearing std. Cells score by
 *    angular distance between (node -> cell) azimuth and the bearing.
 *  - RSSI observations (bearingless SDRs: RTL-SDR, HackRF, ...): a single
 *    receive power carries almost no absolute range information (TX power
 *    unknown), so single detections are only used as a weak "transmitter
 *    is within plausible detection range" prior. The mathematically
 *    useful signal is the PAIRWISE power difference between two spatially
 *    separated detections of the same signal (power-difference-of-arrival):
 *        rssi_i - rssi_j  ~=  -10 n log10(d_i / d_j)
 *    with path-loss exponent n. That ratio constrains position without
 *    knowing TX power, and is what lets moving RTL/HackRF nodes (or one
 *    node logging hits along a drive) tighten the AOU.
 *
 * All math is done on a local equirectangular plane (fine for <50 km).
 * Pure header, no GUI/JSON deps — unit-testable and shared by the
 * Android and Linux builds alike.
 */

namespace predator::aou {

struct BearingObs {
    double lat = 0.0, lon = 0.0;   // observer position
    double bearingDeg = 0.0;       // TRUE bearing to target
    double stdDeg = 10.0;          // 1-sigma bearing uncertainty
    double weight = 1.0;           // time-decay weight (0..1]
};

struct RssiObs {
    double lat = 0.0, lon = 0.0;   // observer position at time of hit
    double rssiDb = 0.0;           // received strength (dB, any consistent ref)
    double weight = 1.0;           // time-decay weight (0..1]
};

struct Cell {
    double lat = 0.0, lon = 0.0;   // cell center
    double p = 0.0;                // probability mass of this cell
};

struct Result {
    bool valid = false;
    double peakLat = 0.0, peakLon = 0.0;
    double cellSizeM = 0.0;
    // Cells sorted by descending probability, truncated to the smallest
    // set covering `coverage` of total probability mass.
    std::vector<Cell> cells;
};

inline double _degWrap(double d) {
    while (d > 180.0) d -= 360.0;
    while (d < -180.0) d += 360.0;
    return d;
}

// Compute the AOU grid for one signal cluster.
//  gridN:      grid is gridN x gridN cells
//  marginM:    grid extends this far beyond the observer bounding box
//  coverage:   probability mass captured by the returned cell set
//  maxCells:   hard cap on returned cells (keeps the JSON push bounded)
inline Result computeAou(const std::vector<BearingObs>& bearings,
                         const std::vector<RssiObs>& rssi,
                         int gridN = 96,
                         double marginM = 12000.0,
                         double coverage = 0.90,
                         int maxCells = 300) {
    Result out;
    // Geometry gate: a lone RSSI detection (or a stack of them from the
    // same spot) cannot localize anything — require either a bearing or
    // two RSSI observations separated enough for the power ratio to mean
    // something. Otherwise report invalid rather than a fake AOU.
    auto sepM = [](double la1, double lo1, double la2, double lo2) {
        double kLat = 111320.0;
        double kLon = 111320.0 * std::cos((la1 + la2) * 0.5 * M_PI / 180.0);
        double dx = (lo2 - lo1) * kLon, dy = (la2 - la1) * kLat;
        return std::sqrt(dx * dx + dy * dy);
    };
    bool rssiGeometry = false;
    for (size_t i = 0; i < rssi.size() && !rssiGeometry; i++) {
        for (size_t j = i + 1; j < rssi.size(); j++) {
            if (sepM(rssi[i].lat, rssi[i].lon, rssi[j].lat, rssi[j].lon) > 150.0) {
                rssiGeometry = true;
                break;
            }
        }
    }
    if (bearings.empty() && !rssiGeometry) return out;

    // ---- Grid extent: observer bounding box + margin ----
    double minLat = 1e9, maxLat = -1e9, minLon = 1e9, maxLon = -1e9;
    auto grow = [&](double la, double lo) {
        minLat = std::min(minLat, la); maxLat = std::max(maxLat, la);
        minLon = std::min(minLon, lo); maxLon = std::max(maxLon, lo);
    };
    for (auto& b : bearings) grow(b.lat, b.lon);
    for (auto& r : rssi) grow(r.lat, r.lon);
    double cLat = (minLat + maxLat) * 0.5, cLon = (minLon + maxLon) * 0.5;
    double kLat = 111320.0;
    double kLon = 111320.0 * std::cos(cLat * M_PI / 180.0);
    if (kLon < 1.0) kLon = 1.0;   // polar degenerate guard
    double halfSpanM = std::max((maxLat - minLat) * kLat, (maxLon - minLon) * kLon) * 0.5
                       + marginM;
    double cellM = (2.0 * halfSpanM) / gridN;

    // ---- Accumulate log-likelihood per cell ----
    std::vector<double> logL((size_t)gridN * gridN, 0.0);

    // Pre-select RSSI pairs (spatially separated, strongest weights first,
    // capped so the per-cell cost stays bounded).
    struct RPair { const RssiObs* a; const RssiObs* b; double w; };
    std::vector<RPair> rpairs;
    for (size_t i = 0; i < rssi.size(); i++) {
        for (size_t j = i + 1; j < rssi.size(); j++) {
            if (sepM(rssi[i].lat, rssi[i].lon, rssi[j].lat, rssi[j].lon) <= 150.0) continue;
            rpairs.push_back({ &rssi[i], &rssi[j], rssi[i].weight * rssi[j].weight });
        }
    }
    std::sort(rpairs.begin(), rpairs.end(),
              [](const RPair& x, const RPair& y) { return x.w > y.w; });
    if (rpairs.size() > 64) rpairs.resize(64);

    const double pathLossN = 3.0;   // suburban-ish path loss exponent
    const double pdoaSigma = 6.0;   // dB — RSSI is noisy; keep this loose

    for (int gy = 0; gy < gridN; gy++) {
        for (int gx = 0; gx < gridN; gx++) {
            double x = (gx + 0.5) * cellM - halfSpanM;   // east meters
            double y = (gy + 0.5) * cellM - halfSpanM;   // north meters
            double acc = 0.0;
            for (auto& b : bearings) {
                double ox = (b.lon - cLon) * kLon, oy = (b.lat - cLat) * kLat;
                double dx = x - ox, dy = y - oy;
                double dist = std::sqrt(dx * dx + dy * dy);
                if (dist < cellM * 0.5) continue;  // cell contains the observer
                double az = std::atan2(dx, dy) * 180.0 / M_PI;  // 0=N, CW
                double diff = _degWrap(az - b.bearingDeg);
                double sig = std::max(2.0, b.stdDeg);
                acc += b.weight * (-0.5 * (diff / sig) * (diff / sig));
            }
            for (auto& pr : rpairs) {
                double ax = (pr.a->lon - cLon) * kLon, ay = (pr.a->lat - cLat) * kLat;
                double bx = (pr.b->lon - cLon) * kLon, by = (pr.b->lat - cLat) * kLat;
                double da = std::max(30.0, std::hypot(x - ax, y - ay));
                double db = std::max(30.0, std::hypot(x - bx, y - by));
                double predicted = -10.0 * pathLossN * std::log10(da / db);
                double meas = pr.a->rssiDb - pr.b->rssiDb;
                double e = (meas - predicted) / pdoaSigma;
                acc += pr.w * (-0.5 * e * e);
            }
            logL[(size_t)gy * gridN + gx] = acc;
        }
    }

    // ---- Normalize (log-sum-exp) into probabilities ----
    double mx = -1e300;
    for (double v : logL) mx = std::max(mx, v);
    double sum = 0.0;
    std::vector<double> p(logL.size());
    for (size_t i = 0; i < logL.size(); i++) {
        p[i] = std::exp(logL[i] - mx);
        sum += p[i];
    }
    if (sum <= 0.0) return out;
    for (auto& v : p) v /= sum;

    // A flat grid (no information) must not render as an AOU: require the
    // peak cell to beat the uniform prior by a healthy factor.
    double uniform = 1.0 / (double)p.size();
    size_t peakIdx = (size_t)(std::max_element(p.begin(), p.end()) - p.begin());
    if (p[peakIdx] < uniform * 8.0) return out;

    // ---- Extract the coverage set ----
    std::vector<size_t> order(p.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return p[a] > p[b]; });
    double cum = 0.0;
    for (size_t oi = 0; oi < order.size() && (int)out.cells.size() < maxCells; oi++) {
        size_t idx = order[oi];
        if (cum >= coverage) break;
        cum += p[idx];
        int gy = (int)(idx / gridN), gx = (int)(idx % gridN);
        double x = (gx + 0.5) * cellM - halfSpanM;
        double y = (gy + 0.5) * cellM - halfSpanM;
        Cell c;
        c.lat = cLat + y / kLat;
        c.lon = cLon + x / kLon;
        c.p = p[idx];
        out.cells.push_back(c);
    }
    out.peakLat = out.cells.empty() ? cLat : out.cells[0].lat;
    out.peakLon = out.cells.empty() ? cLon : out.cells[0].lon;
    out.cellSizeM = cellM;
    out.valid = !out.cells.empty();
    return out;
}

} // namespace predator::aou
