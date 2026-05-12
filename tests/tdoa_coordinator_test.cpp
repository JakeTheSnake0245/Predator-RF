// Build & run:
//   g++ -std=c++17 -O2 -Icore/src tests/tdoa_coordinator_test.cpp -o /tmp/tdoa_test && /tmp/tdoa_test
//
// Pure stdlib, single TU. Mirrors the Python coordinator's behaviour;
// where Python tests use floats, this uses 1e-3 lat/lon tolerance and
// 1m position tolerance to absorb fp64 reordering between the two ports.

#include "predator/tdoa_coordinator.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

using predator::tdoa::Coordinator;
using predator::tdoa::Measurement;
using predator::tdoa::Result;
using predator::tdoa::computeTimingTrust;
using predator::tdoa::gpsFresh;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do {                                            \
    if (cond) { ++g_pass; }                                         \
    else {                                                          \
        ++g_fail;                                                   \
        std::fprintf(stderr, "FAIL %s:%d: %s\n",                    \
                     __FILE__, __LINE__, #cond);                    \
    }                                                               \
} while (0)

#define CHECK_NEAR(a, b, tol) do {                                  \
    const double _a = (a), _b = (b);                                \
    if (std::abs(_a - _b) <= (tol)) { ++g_pass; }                   \
    else {                                                          \
        ++g_fail;                                                   \
        std::fprintf(stderr,                                        \
                     "FAIL %s:%d: %s ≈ %s (%.6g vs %.6g, tol=%.6g)\n",\
                     __FILE__, __LINE__, #a, #b, _a, _b,            \
                     static_cast<double>(tol));                     \
    }                                                               \
} while (0)

static Measurement mk(const std::string& id, int64_t ts_ns,
                      double lat, double lon, double trust = 1.0) {
    Measurement m;
    m.node_id = id;
    m.timestamp_ns = ts_ns;
    m.node_lat = lat;
    m.node_lon = lon;
    m.timing_trust = trust;
    return m;
}

static void test_timing_trust_helpers() {
    // can_do_tdoa branch: clamp to [0.5, 1.0].
    CHECK_NEAR(computeTimingTrust(true, 0.9), 0.9, 1e-9);
    CHECK_NEAR(computeTimingTrust(true, 0.3), 0.5, 1e-9);
    CHECK_NEAR(computeTimingTrust(true, 1.5), 1.0, 1e-9);
    // System-clock branch: clamp to [0.2, 0.5], applied to hw*0.5.
    CHECK_NEAR(computeTimingTrust(false, 0.8), 0.4, 1e-9);
    CHECK_NEAR(computeTimingTrust(false, 0.1), 0.2, 1e-9);
    CHECK_NEAR(computeTimingTrust(false, 1.5), 0.5, 1e-9);
}

static void test_gps_fresh_helper() {
    const int64_t now = 1'700'000'000'000'000'000LL;
    CHECK(gpsFresh(0, now, 60.0));                       // bypass on zero
    CHECK(gpsFresh(now - 30'000'000'000LL, now, 60.0));  // 30s old
    CHECK(!gpsFresh(now - 90'000'000'000LL, now, 60.0)); // 90s old
}

static void test_record_and_distinct() {
    Coordinator c;
    c.recordMeasurement("E1", mk("A", 100, 1.0, 1.0));
    c.recordMeasurement("E1", mk("B", 200, 1.0, 1.001));
    c.recordMeasurement("E1", mk("A", 300, 1.0, 1.0));  // dup A
    CHECK(c.distinctNodes("E1") == 2);
    CHECK(c.pendingSize("E1") == 3);
    CHECK(c.distinctNodes("E2") == 0);
}

static void test_prune_old() {
    Coordinator c;
    c.recordMeasurement("E1", mk("A", 1'000'000'000LL, 0, 0));   // 1s
    c.recordMeasurement("E1", mk("B", 2'000'000'000LL, 0, 0));   // 2s
    c.recordMeasurement("E1", mk("C", 9'000'000'000LL, 0, 0));   // 9s
    // now=10s, max_age=5s -> cutoff=5s; A and B drop, C stays.
    c.pruneOld("E1", 5.0, 10'000'000'000LL);
    CHECK(c.pendingSize("E1") == 1);
    CHECK(c.distinctNodes("E1") == 1);
    // Prune everything.
    c.pruneOld("E1", 0.001, 100'000'000'000LL);
    CHECK(c.pendingSize("E1") == 0);
    CHECK(c.pendingEmitterCount() == 0);
}

static void test_solve_too_few_distinct_restores() {
    Coordinator c;
    c.recordMeasurement("E1", mk("A", 100, 1.0, 1.0));
    c.recordMeasurement("E1", mk("A", 200, 1.0, 1.0));  // same node twice
    auto r = c.solve("E1");
    CHECK(!r.has_value());
    // Measurements restored for the next attempt.
    CHECK(c.pendingSize("E1") == 2);
    CHECK(c.distinctNodes("E1") == 1);
}

static void test_solve_two_node_midpoint() {
    Coordinator c;
    c.recordMeasurement("E1", mk("A", 1'000'000'000LL, 47.0, -122.0, 0.5));
    c.recordMeasurement("E1", mk("B", 1'000'001'000LL, 47.01, -121.99, 0.5));
    auto r = c.solve("E1");
    CHECK(r.has_value());
    if (!r) return;
    CHECK_NEAR(r->estimated_lat, (47.0 + 47.01) / 2.0, 1e-9);
    CHECK_NEAR(r->estimated_lon, (-122.0 + -121.99) / 2.0, 1e-9);
    // 0.3 base * mean(0.5,0.5)=0.5 timing -> 0.15
    CHECK_NEAR(r->location_confidence, 0.15, 1e-9);
    CHECK(r->participating_nodes.size() == 2);
    CHECK(r->time_differences_ns.size() == 1);
    CHECK(r->time_differences_ns.begin()->second == 1000);
    CHECK(r->ellipse_a_m > 0.0);
    CHECK(r->ellipse_b_m > 0.0);
}

static void test_solve_three_node_triangulate_centroid() {
    // Three nodes around the origin emit time-coincident hits — the
    // estimate should land near the geometric centre. dt=0 across all
    // nodes models a perfectly synchronous detection (range_diff=0 ->
    // the LSQ converges to equal-range point ≈ centroid).
    Coordinator c;
    c.recordMeasurement("E1", mk("A", 1'000'000'000LL, 47.000, -122.000));
    c.recordMeasurement("E1", mk("B", 1'000'000'000LL, 47.010, -122.000));
    c.recordMeasurement("E1", mk("C", 1'000'000'000LL, 47.005, -121.990));
    auto r = c.solve("E1");
    CHECK(r.has_value());
    if (!r) return;
    const double exp_lat = (47.000 + 47.010 + 47.005) / 3.0;
    const double exp_lon = (-122.000 + -122.000 + -121.990) / 3.0;
    // dt=0 -> LSQ converges to the equal-range point (circumcenter),
    // which differs from the centroid by O(100m) for asymmetric
    // triangles. Tolerance is 2e-3 deg (~220m) — tight enough to
    // catch a runaway solve, loose enough to absorb the geometric
    // gap between centroid and circumcenter.
    CHECK_NEAR(r->estimated_lat, exp_lat, 2e-3);
    CHECK_NEAR(r->estimated_lon, exp_lon, 2e-3);
    // 3 nodes geometric conf = 0.5 + 3*0.1 = 0.8; trust=1.0 -> 0.8
    CHECK_NEAR(r->location_confidence, 0.8, 1e-9);
    CHECK(r->participating_nodes.size() == 3);
    CHECK(r->time_differences_ns.size() == 2);
}

static void test_solve_three_node_with_time_offset() {
    // Source closer to A than to B and C — emitter ahead of A in time.
    // A at (47.000,-122.100), B at (47.000,-122.000) ≈ 7585 m east,
    // C at (47.010,-122.000) ≈ 7666 m NE. Time diff must be < baseline/c
    // (~25 µs) for the geometry to be physically realizable; pick 5 µs
    // (≈1500 m range diff) so the LSQ stays well-conditioned.
    Coordinator c;
    c.recordMeasurement("E1", mk("A", 0LL,            47.000, -122.100));
    c.recordMeasurement("E1", mk("B", 5'000LL,        47.000, -122.000));
    c.recordMeasurement("E1", mk("C", 5'000LL,        47.010, -122.000));
    auto r = c.solve("E1");
    CHECK(r.has_value());
    if (!r) return;
    // Estimated point should be closer to A than to either B or C.
    auto dist2 = [](double la, double lo, double lb, double lob) {
        const double dy = (la - lb) * 111320.0;
        const double dx = (lo - lob) * 111320.0
            * std::cos(la * M_PI / 180.0);
        return dx * dx + dy * dy;
    };
    const double dA = dist2(r->estimated_lat, r->estimated_lon, 47.000, -122.100);
    const double dB = dist2(r->estimated_lat, r->estimated_lon, 47.000, -122.000);
    const double dC = dist2(r->estimated_lat, r->estimated_lon, 47.010, -122.000);
    CHECK(dA < dB);
    CHECK(dA < dC);
    // Confidence should be the high (0.8) value, not the 2-node fallback.
    CHECK_NEAR(r->location_confidence, 0.8, 1e-9);
    CHECK(r->ellipse_b_m <= r->ellipse_a_m);  // b is the minor axis
}

static void test_ellipse_eccentric_when_collinear_nodes() {
    // Three nodes strung along an east-west line should produce an
    // eccentric ellipse (b/a < 1) because the node-cluster covariance
    // is highly anisotropic.
    Coordinator c;
    c.recordMeasurement("E1", mk("A", 0LL,        47.000, -122.030));
    c.recordMeasurement("E1", mk("B", 0LL,        47.000, -122.000));
    c.recordMeasurement("E1", mk("C", 0LL,        47.000, -121.970));
    auto r = c.solve("E1");
    CHECK(r.has_value());
    if (!r) return;
    CHECK(r->ellipse_b_m < r->ellipse_a_m);
    CHECK(r->ellipse_b_m / r->ellipse_a_m <= 0.5);
    // Theta should be in [0, 180).
    CHECK(r->ellipse_theta_deg >= 0.0);
    CHECK(r->ellipse_theta_deg < 180.0);
}

static void test_solve_clears_pending() {
    Coordinator c;
    c.recordMeasurement("E1", mk("A", 100, 1.0, 1.0));
    c.recordMeasurement("E1", mk("B", 200, 1.0, 1.001));
    auto r = c.solve("E1");
    CHECK(r.has_value());
    CHECK(c.pendingSize("E1") == 0);
    CHECK(c.pendingEmitterCount() == 0);
}

static void test_solve_unknown_emitter_returns_nullopt() {
    Coordinator c;
    auto r = c.solve("nobody");
    CHECK(!r.has_value());
}

static void test_clear() {
    Coordinator c;
    c.recordMeasurement("E1", mk("A", 100, 1.0, 1.0));
    c.recordMeasurement("E2", mk("B", 200, 2.0, 2.0));
    CHECK(c.pendingEmitterCount() == 2);
    c.clear();
    CHECK(c.pendingEmitterCount() == 0);
    CHECK(c.pendingSize("E1") == 0);
}

int main() {
    test_timing_trust_helpers();
    test_gps_fresh_helper();
    test_record_and_distinct();
    test_prune_old();
    test_solve_too_few_distinct_restores();
    test_solve_two_node_midpoint();
    test_solve_three_node_triangulate_centroid();
    test_solve_three_node_with_time_offset();
    test_ellipse_eccentric_when_collinear_nodes();
    test_solve_clears_pending();
    test_solve_unknown_emitter_returns_nullopt();
    test_clear();

    std::cout << "tdoa_coordinator_test: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
