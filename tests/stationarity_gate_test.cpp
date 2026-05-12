// Build & run:
//   g++ -std=c++17 -O2 -Icore/src tests/stationarity_gate_test.cpp -o /tmp/sg_test && /tmp/sg_test

#include "predator/stationarity_gate.h"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <vector>

using predator::stationarity::Config;
using predator::stationarity::FixCandidate;
using predator::stationarity::HistoryPoint;
using predator::stationarity::MotionState;
using predator::stationarity::Verdict;
using predator::stationarity::classifyMotion;
using predator::stationarity::evaluate;
using predator::stationarity::toString;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do {                                         \
    if (cond) { ++g_pass; }                                      \
    else {                                                       \
        ++g_fail;                                                \
        std::fprintf(stderr, "FAIL %s:%d: %s\n",                 \
                     __FILE__, __LINE__, #cond);                 \
    }                                                            \
} while (0)

static FixCandidate fc(double lat, double lon, int64_t ts,
                       double a = 100.0) {
    FixCandidate f;
    f.lat = lat; f.lon = lon; f.timestamp_ns = ts;
    f.ellipse_a_m = a;
    return f;
}

static HistoryPoint hp(double lat, double lon, int64_t ts,
                       double a = 100.0) {
    HistoryPoint h;
    h.lat = lat; h.lon = lon; h.timestamp_ns = ts;
    h.ellipse_a_m = a;
    return h;
}

static void test_evaluate_empty_history_accepts() {
    const auto v = evaluate(fc(47.0, -122.0, 1'000'000'000LL), {});
    CHECK(v.accepted);
    CHECK(v.reason == "bypass_new_track");
    CHECK(v.bypass == "new_track");
    CHECK(v.motion_state == MotionState::Unknown);
}

static void test_evaluate_invalid_coords() {
    auto v1 = evaluate(fc(NAN, -122.0, 1'000'000'000LL), {});
    CHECK(!v1.accepted && v1.reason == "invalid_coordinates");
    auto v2 = evaluate(fc(91.0, -122.0, 1'000'000'000LL), {});
    CHECK(!v2.accepted && v2.reason == "invalid_coordinates");
    auto v3 = evaluate(fc(47.0, -181.0, 1'000'000'000LL), {});
    CHECK(!v3.accepted && v3.reason == "invalid_coordinates");
    auto v5 = evaluate(fc(47.0, -122.0, 0LL), {});
    CHECK(!v5.accepted && v5.reason == "invalid_timestamp");
    auto v6 = evaluate(fc(47.0, -122.0, -5LL), {});
    CHECK(!v6.accepted && v6.reason == "invalid_timestamp");
}

static void test_evaluate_velocity_ok() {
    std::vector<HistoryPoint> hist = {
        hp(47.0000, -122.0000, 1'000'000'000LL),
    };
    // 50m north over 10s = 5 m/s. Well under 100 m/s.
    auto v = evaluate(fc(47.00045, -122.0000, 11'000'000'000LL), hist);
    CHECK(v.accepted);
    CHECK(v.reason == "accepted");
    CHECK(v.implied_velocity_mps > 4.0 && v.implied_velocity_mps < 6.0);
}

static void test_evaluate_velocity_rejected() {
    std::vector<HistoryPoint> hist = {
        hp(47.0000, -122.0000, 1'000'000'000LL),
    };
    // 100km jump in 10s -> 10000 m/s. Reject.
    auto v = evaluate(fc(47.9, -122.0, 11'000'000'000LL), hist);
    CHECK(!v.accepted);
    CHECK(v.reason == "velocity_exceeds_limit");
    CHECK(v.implied_velocity_mps > 100.0);
}

static void test_evaluate_dt_floor_bypasses_velocity() {
    // Python parity: dt < dt_floor BYPASSES the velocity check entirely
    // (it does not clamp dt and compute velocity anyway). A 150 m
    // jump 1 ns later must accept with bypass="dt_floor" and
    // implied_velocity_mps unset (<0).
    std::vector<HistoryPoint> hist = {
        hp(47.0000, -122.0000, 1'000'000'000LL),
    };
    auto v = evaluate(fc(47.00135, -122.0000, 1'000'000'001LL), hist);
    CHECK(v.accepted);
    CHECK(v.reason == "bypass_dt_floor");
    CHECK(v.bypass == "dt_floor");
    CHECK(v.implied_velocity_mps < 0.0);
}

static void test_evaluate_garbage_history_accepts() {
    // Last history point has NaN lat. haversine returns NaN, NaN > vmax
    // is false in IEEE754 so we accept (Python behaviour). Defensive
    // coverage rather than aspirational.
    std::vector<HistoryPoint> hist = { hp(NAN, -122.0, 1'000'000'000LL) };
    auto v = evaluate(fc(47.0, -122.0, 11'000'000'000LL), hist);
    CHECK(v.accepted);
}

static void test_classify_motion_too_short() {
    std::vector<HistoryPoint> hist = { hp(47.0, -122.0, 1, 100) };
    CHECK(classifyMotion(hist) == MotionState::Unknown);  // <2 points
}

static void test_classify_motion_stationary() {
    // Points clustered within ~1m of centroid, ellipse_a_m=100.
    // RMS << 1.0 * 100 -> stationary.
    std::vector<HistoryPoint> hist = {
        hp(47.00001, -122.00000, 1, 100),
        hp(47.00000, -122.00001, 2, 100),
        hp(47.00000, -122.00000, 3, 100),
        hp(47.00001, -122.00001, 4, 100),
        hp(47.00000, -122.00000, 5, 100),
        hp(47.00001, -122.00001, 6, 100),
    };
    CHECK(classifyMotion(hist) == MotionState::Stationary);
}

static void test_classify_motion_mobile() {
    // Points spread over ~3 km, ellipse_a_m=50 -> RMS/avg >> 3.0 mobile cutoff.
    std::vector<HistoryPoint> hist = {
        hp(47.000, -122.000, 1, 50),
        hp(47.005, -122.000, 2, 50),
        hp(47.010, -122.000, 3, 50),
        hp(47.015, -122.000, 4, 50),
        hp(47.020, -122.000, 5, 50),
        hp(47.025, -122.000, 6, 50),
    };
    CHECK(classifyMotion(hist) == MotionState::Mobile);
}

static void test_classify_motion_hysteresis() {
    // Need RMS in (1.0*ellipse, 3.0*ellipse]. With ellipse_a_m=100,
    // target RMS ~200 m. Five points along a north line at 0, 100,
    // 200, 300, 400 m gives centroid at 200 m, RMS = sqrt((200^2 +
    // 100^2 + 0 + 100^2 + 200^2)/5) = sqrt(20000) ≈ 141 m. Ratio 1.41
    // — between 1.0 and 3.0, so hysteresis kicks in.
    // 100 m north of 47.000 ≈ +0.0009 deg.
    std::vector<HistoryPoint> hist = {
        hp(47.0000, -122.0, 1, 100),
        hp(47.0009, -122.0, 2, 100),
        hp(47.0018, -122.0, 3, 100),
        hp(47.0027, -122.0, 4, 100),
        hp(47.0036, -122.0, 5, 100),
    };
    CHECK(classifyMotion(hist, MotionState::Mobile) == MotionState::Mobile);
    CHECK(classifyMotion(hist, MotionState::Stationary)
          == MotionState::Stationary);
    CHECK(classifyMotion(hist, MotionState::Unknown) == MotionState::Unknown);
}

static void test_classify_motion_no_ellipse_uses_fallback() {
    // Python parity: when no history point sets ellipse_a_m, the
    // fallback_ellipse_m (250 m default) drives the comparison.
    // Six identical points -> RMS=0, 0 <= 1.0*250 -> STATIONARY.
    std::vector<HistoryPoint> hist = {
        hp(47.000, -122.000, 1, -1.0),
        hp(47.000, -122.000, 2, -1.0),
        hp(47.000, -122.000, 3, -1.0),
        hp(47.000, -122.000, 4, -1.0),
        hp(47.000, -122.000, 5, -1.0),
    };
    CHECK(classifyMotion(hist) == MotionState::Stationary);
}

static void test_classify_motion_zero_ellipse_uses_fallback() {
    // Python: ellipse_avg <= 0 also falls back. Zero ellipses with
    // fallback=250 means 6 identical points -> STATIONARY.
    std::vector<HistoryPoint> hist = {
        hp(47.000, -122.000, 1, 0.0),
        hp(47.000, -122.000, 2, 0.0),
        hp(47.000, -122.000, 3, 0.0),
        hp(47.000, -122.000, 4, 0.0),
        hp(47.000, -122.000, 5, 0.0),
    };
    // ellipse_avg = (0+0+0+0+0)/5 = 0 -> swapped to fallback 250.
    CHECK(classifyMotion(hist) == MotionState::Stationary);
}

static void test_to_string() {
    CHECK(std::string(toString(MotionState::Stationary)) == "stationary");
    CHECK(std::string(toString(MotionState::Mobile)) == "mobile");
    CHECK(std::string(toString(MotionState::Unknown)) == "unknown");
}

int main() {
    test_evaluate_empty_history_accepts();
    test_evaluate_invalid_coords();
    test_evaluate_velocity_ok();
    test_evaluate_velocity_rejected();
    test_evaluate_dt_floor_bypasses_velocity();
    test_evaluate_garbage_history_accepts();
    test_classify_motion_too_short();
    test_classify_motion_stationary();
    test_classify_motion_mobile();
    test_classify_motion_hysteresis();
    test_classify_motion_no_ellipse_uses_fallback();
    test_classify_motion_zero_ellipse_uses_fallback();
    test_to_string();

    std::cout << "stationarity_gate_test: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
