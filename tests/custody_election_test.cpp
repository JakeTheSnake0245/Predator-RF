// Standalone test runner for predator::custody::Elector.
//
// Build: g++ -std=c++17 -O2 -I core/src tests/custody_election_test.cpp \
//             -o /tmp/custody_election_test
// Run:   /tmp/custody_election_test            (asserts, no output)
//        /tmp/custody_election_test --json     (emit per-scenario JSON)
//        /tmp/custody_election_test --fixture tests/fixtures/custody_scenarios.json
//
// The --fixture mode is consumed by scripts/test_custody_parity.py to
// drive the same scenarios through the Python elector and diff outputs.

#include "predator/custody_election.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "json.hpp"

using predator::custody::Decision;
using predator::custody::Elector;
using predator::custody::NodeInput;
using predator::custody::Score;
using predator::custody::TrackInput;

// ── Tiny test framework ──────────────────────────────────────────────────

static int g_pass = 0;
static int g_fail = 0;

#define TEST(name) static void name()
#define EXPECT(cond) do {                                                     \
    if (!(cond)) {                                                            \
        std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        ++g_fail;                                                             \
    } else { ++g_pass; }                                                      \
} while (0)
#define EXPECT_EQ(a, b) do {                                                  \
    auto __va = (a); auto __vb = (b);                                         \
    if (!(__va == __vb)) {                                                    \
        std::ostringstream __oss;                                             \
        __oss << "  FAIL " << __FILE__ << ":" << __LINE__                     \
              << ": " << #a << " == " << #b                                   \
              << " (got " << __va << " vs " << __vb << ")\n";                 \
        std::fputs(__oss.str().c_str(), stderr);                              \
        ++g_fail;                                                             \
    } else { ++g_pass; }                                                      \
} while (0)

// ── Helpers ──────────────────────────────────────────────────────────────

static NodeInput makeNode(const std::string& id, double trust = 0.7) {
    NodeInput n;
    n.node_id = id;
    n.gps_synchronized = true;
    n.has_gps_location = true;
    n.gps_lat = 47.6062;
    n.gps_lon = -122.3321;
    // 10 s before kTestNowNs — fresh enough that the stale-GPS hard
    // gate (>300 s) doesn't fire on high-threat tracks. Tests that
    // need a stale fix override this explicitly.
    n.gps_updated_ns = 2'000'000'000'000'000'000LL - 10'000'000'000LL;
    n.sensitivity_trust = 0.8;
    n.trust_score = trust;
    n.thermal_throttling_active = false;
    return n;
}

static TrackInput makeTrack(const std::string& id = "trk-1",
                             const std::string& threat = "low") {
    TrackInput t;
    t.track_id = id;
    t.threat_level = threat;
    t.has_estimated_position = true;
    t.estimated_lat = 47.6062;
    t.estimated_lon = -122.3321;
    return t;
}

static bool contains(const std::vector<std::string>& v, const std::string& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

// Use a wall-clock value far in the future so all subtractions stay
// positive — matches the convention used in test_custody_election.py.
static constexpr int64_t kTestNowNs = 2'000'000'000'000'000'000LL;

// ── Tests (mirror the most load-bearing Python cases) ────────────────────

TEST(test_no_nodes_yields_no_primary) {
    Elector e;
    auto d = e.elect(makeTrack(), {}, kTestNowNs);
    EXPECT(d.primary.empty());
    EXPECT(d.tasked_nodes.empty());
    EXPECT(!d.reason.empty());
}

TEST(test_single_node_elected_as_primary) {
    Elector e;
    auto d = e.elect(makeTrack(), {makeNode("A")}, kTestNowNs);
    EXPECT_EQ(d.primary, std::string("A"));
    EXPECT(d.backups.empty());
    EXPECT_EQ((int)d.tasked_nodes.size(), 1);
}

TEST(test_higher_trust_wins) {
    Elector e;
    auto d = e.elect(makeTrack(),
                     {makeNode("A", 0.4), makeNode("B", 0.95)},
                     kTestNowNs);
    EXPECT_EQ(d.primary, std::string("B"));
    EXPECT_EQ(d.backups.size(), (size_t)1);
    EXPECT_EQ(d.backups.front(), std::string("A"));
}

TEST(test_deterministic_tiebreak_by_node_id) {
    Elector e;
    auto d = e.elect(makeTrack(),
                     {makeNode("Z", 0.7), makeNode("A", 0.7), makeNode("M", 0.7)},
                     kTestNowNs);
    EXPECT_EQ(d.primary, std::string("A"));   // alpha-sort tiebreak
}

TEST(test_tdoa_high_threat_rejects_unsynced_node) {
    Elector e;
    auto bad = makeNode("A");
    bad.gps_synchronized = false;
    auto good = makeNode("B");
    auto d = e.elect(makeTrack("t", "high"), {bad, good}, kTestNowNs);
    EXPECT_EQ(d.primary, std::string("B"));
    // bad scored 0 with the canonical reason
    bool found_reason = false;
    for (const auto& s : d.scores) {
        if (s.node_id == "A") {
            EXPECT_EQ(s.total, 0.0);
            EXPECT_EQ(s.rejected_reason, std::string("tdoa_threat_requires_gps_sync"));
            found_reason = true;
        }
    }
    EXPECT(found_reason);
}

TEST(test_stale_gps_rejected_for_high_threat) {
    Elector e;
    auto stale = makeNode("A");
    stale.gps_updated_ns = kTestNowNs - 600LL * 1'000'000'000LL;  // 600 s old
    auto fresh = makeNode("B");
    fresh.gps_updated_ns = kTestNowNs - 10LL * 1'000'000'000LL;
    auto d = e.elect(makeTrack("t", "critical"), {stale, fresh}, kTestNowNs);
    EXPECT_EQ(d.primary, std::string("B"));
    for (const auto& s : d.scores) {
        if (s.node_id == "A") {
            EXPECT(s.rejected_reason.find("gps_fix_stale_") == 0);
        }
    }
}

TEST(test_missing_decoder_hard_gates) {
    Elector e;
    auto a = makeNode("A");
    a.available_decoders = {"rtl433"};
    auto b = makeNode("B");
    b.available_decoders = {"p25", "rtl433"};
    auto t = makeTrack();
    t.protocol = "p25";
    auto d = e.elect(t, {a, b}, kTestNowNs);
    EXPECT_EQ(d.primary, std::string("B"));
    for (const auto& s : d.scores) {
        if (s.node_id == "A") {
            EXPECT_EQ(s.rejected_reason, std::string("missing_decoder_p25"));
        }
    }
}

TEST(test_thermal_throttling_halves_score) {
    Elector e;
    auto cool = makeNode("A", 0.7);
    auto hot  = makeNode("B", 0.9);
    hot.thermal_throttling_active = true;
    auto d = e.elect(makeTrack(), {cool, hot}, kTestNowNs);
    // hot's higher trust shouldn't beat cool because of the 0.5x penalty
    EXPECT_EQ(d.primary, std::string("A"));
}

TEST(test_handover_starts_on_primary_change) {
    Elector e;
    auto t = makeTrack();
    e.elect(t, {makeNode("A", 0.9), makeNode("B", 0.5)}, kTestNowNs);
    auto d = e.elect(t, {makeNode("A", 0.4), makeNode("B", 0.95)},
                     kTestNowNs + 1'000'000'000LL);
    EXPECT_EQ(d.primary, std::string("B"));
    EXPECT_EQ(d.handover_from, std::string("A"));
    EXPECT(contains(d.tasked_nodes, "A"));
}

TEST(test_handover_persists_across_multiple_elections) {
    // The exact regression from item #2 round 2 — overlap must span
    // every re-election that falls inside the deadline, not just the
    // tick where the handover starts.
    Elector e(/*k_total=*/2);
    auto t = makeTrack();
    std::vector<NodeInput> nodes_a = {
        makeNode("A", 0.9), makeNode("B", 0.5), makeNode("C", 0.7)};
    e.elect(t, nodes_a, kTestNowNs);
    std::vector<NodeInput> nodes_b = {
        makeNode("A", 0.4), makeNode("B", 0.95), makeNode("C", 0.85)};
    auto d_h = e.elect(t, nodes_b, kTestNowNs + 1'000'000'000LL);
    EXPECT_EQ(d_h.primary, std::string("B"));
    EXPECT_EQ(d_h.handover_from, std::string("A"));
    int64_t deadline = d_h.handover_until_ns;

    auto d_mid = e.elect(t, nodes_b, kTestNowNs + 5'000'000'000LL);
    EXPECT_EQ(d_mid.primary, std::string("B"));
    EXPECT_EQ(d_mid.handover_from, std::string("A"));
    EXPECT(contains(d_mid.tasked_nodes, "A"));
    EXPECT_EQ(d_mid.handover_until_ns, deadline);   // NOT reset

    auto d_after = e.elect(t, nodes_b, kTestNowNs + 22'000'000'000LL);
    EXPECT(d_after.handover_from.empty());
    EXPECT(!contains(d_after.tasked_nodes, "A"));
    EXPECT(contains(d_after.stand_down, "A"));
}

TEST(test_handover_skipped_when_old_primary_disappeared) {
    Elector e;
    auto t = makeTrack();
    e.elect(t, {makeNode("A", 0.9), makeNode("B", 0.5)}, kTestNowNs);
    // A vanishes from the fleet; only B remains.
    auto d = e.elect(t, {makeNode("B", 0.5)},
                     kTestNowNs + 1'000'000'000LL);
    EXPECT_EQ(d.primary, std::string("B"));
    EXPECT(d.handover_from.empty());
    EXPECT(!contains(d.tasked_nodes, "A"));
}

TEST(test_forget_releases_cache) {
    Elector e;
    auto t = makeTrack("trk-x");
    e.elect(t, {makeNode("A")}, kTestNowNs);
    EXPECT_EQ(e.stats().tracks_in_cache, (size_t)1);
    e.forget("trk-x");
    EXPECT_EQ(e.stats().tracks_in_cache, (size_t)0);
}

TEST(test_load_score_spreads_custody) {
    Elector e;
    // Two equal-trust nodes; A has 3 active tracks, B has 0. Load
    // weight (0.10) should tip the election to B.
    auto a = makeNode("A", 0.7);
    auto b = makeNode("B", 0.7);
    auto d = e.elect(makeTrack(), {a, b}, kTestNowNs,
                     {{"A", 3}, {"B", 0}});
    EXPECT_EQ(d.primary, std::string("B"));
}

TEST(test_on_change_fires_only_when_primary_changes) {
    Elector e;
    int fires = 0;
    e.setOnChange([&](const Decision&) { ++fires; });
    auto t = makeTrack();
    e.elect(t, {makeNode("A", 0.9), makeNode("B", 0.5)}, kTestNowNs);
    EXPECT_EQ(fires, 1);  // empty -> A
    e.elect(t, {makeNode("A", 0.9), makeNode("B", 0.5)},
            kTestNowNs + 1'000'000'000LL);
    EXPECT_EQ(fires, 1);  // A -> A, no change
    e.elect(t, {makeNode("A", 0.4), makeNode("B", 0.95)},
            kTestNowNs + 2'000'000'000LL);
    EXPECT_EQ(fires, 2);  // A -> B
}

// ── Fixture-driven mode for the parity harness ───────────────────────────

static nlohmann::json decisionToJson(const Decision& d) {
    nlohmann::json j;
    j["track_id"]          = d.track_id;
    j["primary"]           = d.primary;
    j["backups"]           = d.backups;
    j["tasked_nodes"]      = d.tasked_nodes;
    j["stand_down"]        = d.stand_down;
    j["handover_from"]     = d.handover_from;
    j["handover_until_ns"] = d.handover_until_ns;
    nlohmann::json scores = nlohmann::json::array();
    for (const auto& s : d.scores) {
        nlohmann::json sj;
        sj["node_id"] = s.node_id;
        // Round to 4 decimals to match Python `round(x, 4)` in to_dict().
        auto round4 = [](double x) {
            return std::round(x * 10000.0) / 10000.0;
        };
        sj["total"] = round4(s.total);
        nlohmann::json comp;
        for (const auto& kv : s.components) comp[kv.first] = round4(kv.second);
        sj["components"] = comp;
        sj["rejected_reason"] = s.rejected_reason;
        scores.push_back(sj);
    }
    j["scores"] = scores;
    j["reason"] = d.reason;
    return j;
}

static TrackInput trackFromJson(const nlohmann::json& j) {
    TrackInput t;
    t.track_id     = j.value("track_id", "");
    t.threat_level = j.value("threat_level", "low");
    if (j.contains("estimated_lat") && !j["estimated_lat"].is_null()) {
        t.has_estimated_position = true;
        t.estimated_lat = j["estimated_lat"].get<double>();
        t.estimated_lon = j["estimated_lon"].get<double>();
    }
    t.protocol = j.value("protocol", "");
    if (j.contains("detecting_nodes")) {
        for (const auto& d : j["detecting_nodes"]) t.detecting_nodes.push_back(d);
    }
    return t;
}

static NodeInput nodeFromJson(const nlohmann::json& j) {
    NodeInput n;
    n.node_id = j.value("node_id", "");
    n.gps_synchronized = j.value("gps_synchronized", false);
    if (j.contains("gps_lat") && !j["gps_lat"].is_null()) {
        n.has_gps_location = true;
        n.gps_lat = j["gps_lat"].get<double>();
        n.gps_lon = j["gps_lon"].get<double>();
    }
    n.gps_updated_ns = j.value("gps_updated_ns", (int64_t)0);
    n.sensitivity_trust = j.value("sensitivity_trust", 0.5);
    n.trust_score = j.value("trust_score", 0.5);
    if (j.contains("available_decoders")) {
        for (const auto& d : j["available_decoders"]) n.available_decoders.push_back(d);
    }
    n.thermal_throttling_active = j.value("thermal_throttling_active", false);
    return n;
}

static int runFixture(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "fixture not found: %s\n", path.c_str());
        return 2;
    }
    nlohmann::json fixture; in >> fixture;
    Elector e(fixture.value("k_total", 3),
              fixture.value("handover_overlap_s", 15.0),
              fixture.value("stale_gps_after_s", 300.0));
    nlohmann::json out = nlohmann::json::array();
    for (const auto& step : fixture["steps"]) {
        TrackInput t = trackFromJson(step["track"]);
        std::vector<NodeInput> nodes;
        for (const auto& n : step["nodes"]) nodes.push_back(nodeFromJson(n));
        std::map<std::string, int> loads;
        if (step.contains("node_loads")) {
            for (auto it = step["node_loads"].begin();
                 it != step["node_loads"].end(); ++it) {
                loads[it.key()] = it.value().get<int>();
            }
        }
        int64_t now_ns = step.value("now_ns", (int64_t)kTestNowNs);
        out.push_back(decisionToJson(e.elect(t, nodes, now_ns, loads)));
    }
    std::cout << out.dump(2) << std::endl;
    return 0;
}

// ── Entry point ──────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--fixture") {
        return runFixture(argv[2]);
    }

    test_no_nodes_yields_no_primary();
    test_single_node_elected_as_primary();
    test_higher_trust_wins();
    test_deterministic_tiebreak_by_node_id();
    test_tdoa_high_threat_rejects_unsynced_node();
    test_stale_gps_rejected_for_high_threat();
    test_missing_decoder_hard_gates();
    test_thermal_throttling_halves_score();
    test_handover_starts_on_primary_change();
    test_handover_persists_across_multiple_elections();
    test_handover_skipped_when_old_primary_disappeared();
    test_forget_releases_cache();
    test_load_score_spreads_custody();
    test_on_change_fires_only_when_primary_changes();

    std::printf("custody_election_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
