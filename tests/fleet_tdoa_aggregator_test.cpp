// Build & run:
//   g++ -std=c++17 -O2 -Icore/src tests/fleet_tdoa_aggregator_test.cpp -o /tmp/agg_test && /tmp/agg_test

#include "predator/fleet_tdoa_aggregator.h"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using predator::tdoa::AggregatorConfig;
using predator::tdoa::FleetTDOAAggregator;
using predator::tdoa::PeerObservation;
using predator::tdoa::Result;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do {                                       \
    if (cond) { ++g_pass; }                                    \
    else {                                                     \
        ++g_fail;                                              \
        std::fprintf(stderr, "FAIL %s:%d: %s\n",               \
                     __FILE__, __LINE__, #cond);               \
    }                                                          \
} while (0)

static PeerObservation obs(const std::string& node, int64_t ts_ns,
                           double freq_hz, double lat, double lon,
                           double trust = 1.0,
                           int64_t gps_ts = 0) {
    PeerObservation o;
    o.node_id = node;
    o.timestamp_ns = ts_ns;
    o.frequency_hz = freq_hz;
    o.node_lat = lat;
    o.node_lon = lon;
    o.timing_trust = trust;
    o.gps_updated_ns = gps_ts;
    return o;
}

static void test_emitter_key_quantization() {
    // Default 1 kHz buckets.
    const auto k1 = FleetTDOAAggregator::emitterKey(433920000.0, 1000.0);
    const auto k2 = FleetTDOAAggregator::emitterKey(433920400.0, 1000.0);
    const auto k3 = FleetTDOAAggregator::emitterKey(433921000.0, 1000.0);
    CHECK(k1 == k2);          // both round to 433920000
    CHECK(k1 != k3);          // separate bucket
}

static void test_ingest_rejects_bad_inputs() {
    FleetTDOAAggregator a;
    const int64_t now = 1'700'000'000'000'000'000LL;
    CHECK(!a.ingest(obs("A", now, 0.0,    47.0, -122.0), now));   // freq 0
    CHECK(!a.ingest(obs("A", now, NAN,    47.0, -122.0), now));   // freq NaN
    CHECK(!a.ingest(obs("A", now, 433e6,  91.0, -122.0), now));   // bad lat
    CHECK(!a.ingest(obs("A", now, 433e6,  47.0, -181.0), now));   // bad lon
    CHECK(!a.ingest(obs("A", now, 433e6,  NAN,  -122.0), now));   // NaN lat
    CHECK(a.ingested() == 0);
}

static void test_ingest_stale_gps_dropped() {
    AggregatorConfig cfg;
    cfg.gps_max_age_s = 60.0;
    FleetTDOAAggregator a(cfg);
    const int64_t now = 1'700'000'000'000'000'000LL;
    // GPS timestamp 5 min stale.
    auto o = obs("A", now, 433e6, 47.0, -122.0, 1.0,
                 now - 5LL * 60LL * 1'000'000'000LL);
    CHECK(!a.ingest(o, now));
    CHECK(a.droppedStaleGps() == 1);
    CHECK(a.ingested() == 0);
}

static void test_ingest_fresh_gps_accepted() {
    FleetTDOAAggregator a;
    const int64_t now = 1'700'000'000'000'000'000LL;
    auto o = obs("A", now, 433e6, 47.0, -122.0, 1.0,
                 now - 5'000'000'000LL);  // 5s old
    CHECK(a.ingest(o, now));
    CHECK(a.ingested() == 1);
}

static void test_solve_below_distinct_threshold() {
    FleetTDOAAggregator a;
    const int64_t now = 1'700'000'000'000'000'000LL;
    a.ingest(obs("A", now, 433920000, 47.0,    -122.0), now);
    a.ingest(obs("A", now, 433920000, 47.0,    -122.0), now);  // dup A
    auto fixes = a.tick(now);
    CHECK(fixes.empty());
    CHECK(a.fixes() == 0);
    CHECK(a.pendingEmitterCount() == 1);
}

static void test_solve_two_distinct_emits_fix() {
    FleetTDOAAggregator a;
    const int64_t now = 1'700'000'000'000'000'000LL;
    a.ingest(obs("A", now,         433920000, 47.000, -122.000), now);
    a.ingest(obs("B", now + 1'000, 433920000, 47.010, -121.990), now);
    auto fixes = a.tick(now);
    CHECK(fixes.size() == 1);
    CHECK(a.fixes() == 1);
    if (!fixes.empty()) {
        CHECK(fixes[0].participating_nodes.size() == 2);
        // Midpoint check (2-node fallback).
        CHECK(std::abs(fixes[0].estimated_lat - 47.005) < 1e-6);
    }
}

static void test_solve_emits_via_callback() {
    FleetTDOAAggregator a;
    int cb_count = 0;
    Result captured;
    a.setOnFix([&](const Result& r) {
        ++cb_count;
        captured = r;
    });
    const int64_t now = 1'700'000'000'000'000'000LL;
    a.ingest(obs("A", now,         433920000, 47.000, -122.000), now);
    a.ingest(obs("B", now + 1'000, 433920000, 47.010, -121.990), now);
    a.tick(now);
    CHECK(cb_count == 1);
    CHECK(captured.participating_nodes.size() == 2);
}

static void test_solve_cooldown_suppresses_thrash() {
    AggregatorConfig cfg;
    cfg.solve_cooldown_s = 5.0;
    FleetTDOAAggregator a(cfg);
    const int64_t now = 1'700'000'000'000'000'000LL;
    a.ingest(obs("A", now,         433920000, 47.000, -122.000), now);
    a.ingest(obs("B", now + 1'000, 433920000, 47.010, -121.990), now);
    auto f1 = a.tick(now);
    CHECK(f1.size() == 1);
    // Bursty re-ingest 1 second later — cooldown should suppress.
    a.ingest(obs("A", now + 1'000'000'000LL, 433920000, 47.000, -122.000),
             now + 1'000'000'000LL);
    a.ingest(obs("B", now + 1'000'000'001LL, 433920000, 47.010, -121.990),
             now + 1'000'000'000LL);
    auto f2 = a.tick(now + 1'000'000'000LL);
    CHECK(f2.empty());
    // After 5s the cooldown lifts.
    a.ingest(obs("A", now + 6'000'000'000LL, 433920000, 47.000, -122.000),
             now + 6'000'000'000LL);
    a.ingest(obs("B", now + 6'000'000'001LL, 433920000, 47.010, -121.990),
             now + 6'000'000'000LL);
    auto f3 = a.tick(now + 6'000'000'000LL);
    CHECK(f3.size() == 1);
}

static void test_freq_quantization_separates_emitters() {
    FleetTDOAAggregator a;
    const int64_t now = 1'700'000'000'000'000'000LL;
    // Two genuinely different emitters, 100 kHz apart.
    a.ingest(obs("A", now, 433900000, 47.000, -122.000), now);
    a.ingest(obs("B", now, 433900000, 47.010, -121.990), now);
    a.ingest(obs("A", now, 434000000, 47.000, -122.000), now);
    a.ingest(obs("B", now, 434000000, 47.010, -121.990), now);
    auto fixes = a.tick(now);
    CHECK(fixes.size() == 2);
    CHECK(a.pendingEmitterCount() == 0);  // both popped on solve
}

static void test_ttl_drops_old_measurements() {
    AggregatorConfig cfg;
    cfg.measurement_ttl_s = 5.0;
    FleetTDOAAggregator a(cfg);
    const int64_t base = 1'700'000'000'000'000'000LL;
    a.ingest(obs("A", base, 433920000, 47.000, -122.000), base);
    a.ingest(obs("B", base, 433920000, 47.010, -121.990), base);
    // Tick 10 seconds later with no new ingests; both measurements
    // should have been pruned and no fix should fire.
    auto fixes = a.tick(base + 10'000'000'000LL);
    CHECK(fixes.empty());
}

static void test_clear_resets_state() {
    FleetTDOAAggregator a;
    const int64_t now = 1'700'000'000'000'000'000LL;
    a.ingest(obs("A", now, 433920000, 47.000, -122.000), now);
    a.ingest(obs("B", now, 433920000, 47.010, -121.990), now);
    a.tick(now);
    a.clear();
    // After clear, a follow-up matched ingest+tick should NOT be
    // suppressed by the prior solve's cooldown.
    a.ingest(obs("A", now + 100'000'000LL, 433920000, 47.000, -122.000),
             now + 100'000'000LL);
    a.ingest(obs("B", now + 100'000'001LL, 433920000, 47.010, -121.990),
             now + 100'000'000LL);
    auto f = a.tick(now + 100'000'000LL);
    CHECK(f.size() == 1);
}

int main() {
    test_emitter_key_quantization();
    test_ingest_rejects_bad_inputs();
    test_ingest_stale_gps_dropped();
    test_ingest_fresh_gps_accepted();
    test_solve_below_distinct_threshold();
    test_solve_two_distinct_emits_fix();
    test_solve_emits_via_callback();
    test_solve_cooldown_suppresses_thrash();
    test_freq_quantization_separates_emitters();
    test_ttl_drops_old_measurements();
    test_clear_resets_state();

    std::cout << "fleet_tdoa_aggregator_test: " << g_pass
              << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
