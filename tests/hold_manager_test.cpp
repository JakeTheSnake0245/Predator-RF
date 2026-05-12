// Standalone unit tests for predator::hold::HoldManager.
//
// Build:  g++ -std=c++17 -O2 -Icore/src tests/hold_manager_test.cpp -o /tmp/hmt
// Run:    /tmp/hmt
//
// The manager is intentionally header-only and free of sigpath / ImGui
// deps so this test runner needs nothing but the project's bundled
// nlohmann/json header (core/src/json.hpp) and a C++17 compiler.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "predator/hold_manager.h"

using namespace predator::hold;

// ─── Tiny test harness ──────────────────────────────────────────────
static int g_pass = 0;
static int g_fail = 0;
#define CHECK(cond) do {                                                 \
    if (cond) { ++g_pass; }                                              \
    else {                                                               \
        ++g_fail;                                                        \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);\
    }                                                                    \
} while (0)

// ─── Mock VFO interface (records every call) ────────────────────────
struct MockVfoOps {
    struct CreateCall  { std::string id; std::string vfoName; double offset; double bw; DecoderKind dec; };
    struct DestroyCall { std::string vfoName; };
    struct AnchorCall  { std::string vfoName; double offset; double bw; };

    std::vector<CreateCall>  creates;
    std::vector<DestroyCall> destroys;
    std::vector<AnchorCall>  anchors;
    bool createShouldFail = false;

    HoldManager::CreateFn  createFn() {
        return [this](const HoldEntry& e, const std::string& v, double off) {
            if (createShouldFail) return false;
            creates.push_back({e.id, v, off, e.bandwidth_hz, e.decoder});
            return true;
        };
    }
    HoldManager::DestroyFn destroyFn() {
        return [this](const std::string& v) { destroys.push_back({v}); };
    }
    HoldManager::AnchorFn  anchorFn() {
        return [this](const std::string& v, double off, double bw) {
            anchors.push_back({v, off, bw});
        };
    }
    void clearLog() { creates.clear(); destroys.clear(); anchors.clear(); }
};

// ─── Tests ──────────────────────────────────────────────────────────

static void test_add_and_size_limits() {
    HoldManager m;
    CHECK(m.size() == 0);
    CHECK(m.maxEntries() == 8);

    auto id1 = m.add(433.92e6, 12500.0, DecoderKind::Native_RTL433, "ISM 433", 1);
    CHECK(!id1.empty());
    CHECK(m.size() == 1);
    CHECK(m.entries()[0].decoder == DecoderKind::Native_RTL433);
    CHECK(m.entries()[0].label == "ISM 433");

    // Duplicate within tolerance is rejected.
    auto dup = m.add(433.92e6 + 50.0, 12500.0, DecoderKind::Radio_NBFM, "", 2);
    CHECK(dup.empty());
    CHECK(m.size() == 1);

    // Just outside tolerance is accepted.
    m.setDuplicateToleranceHz(100.0);
    auto near = m.add(433.92e6 + 250.0, 12500.0, DecoderKind::Radio_NBFM, "", 3);
    CHECK(!near.empty());
    CHECK(m.size() == 2);

    // Invalid inputs rejected.
    CHECK(m.add(0.0, 12500.0, DecoderKind::Radio_NBFM, "", 4).empty());
    CHECK(m.add(-1.0, 12500.0, DecoderKind::Radio_NBFM, "", 5).empty());
    CHECK(m.add(100e6, 0.0, DecoderKind::Radio_NBFM, "", 6).empty());
    CHECK(m.add(std::nan(""), 12500.0, DecoderKind::Radio_NBFM, "", 7).empty());
    CHECK(m.add(100e6, std::nan(""), DecoderKind::Radio_NBFM, "", 8).empty());
    CHECK(m.size() == 2);

    // Cap at maxEntries.
    m.setMaxEntries(3);
    CHECK(!m.add(146.52e6, 12500.0, DecoderKind::Radio_NBFM, "2m calling", 9).empty());
    CHECK(m.size() == 3);
    CHECK(m.add(1090.0e6, 2.5e6, DecoderKind::Native_ADSB, "ADS-B", 10).empty());
    CHECK(m.size() == 3);
}

static void test_lifecycle_in_and_out_of_band() {
    HoldManager m;
    auto id = m.add(433.92e6, 12500.0, DecoderKind::Native_RTL433, "ISM", 1);
    CHECK(!id.empty());

    MockVfoOps ops;

    // Source @ 433.92 MHz, 2.4 MHz wide → entry sits dead centre.
    auto s1 = m.tick(433.92e6, 2.4e6, 100,
                     ops.createFn(), ops.destroyFn(), ops.anchorFn());
    CHECK(s1.created == 1);
    CHECK(s1.anchored == 1);
    CHECK(s1.destroyed == 0);
    CHECK(ops.creates.size() == 1);
    CHECK(ops.creates[0].vfoName == "Predator H" + id);
    CHECK(std::fabs(ops.creates[0].offset - 0.0) < 1.0);
    CHECK(m.runtimeFor(id).in_band);
    CHECK(m.runtimeFor(id).vfo_active);

    // Source moves to 1090 MHz → entry now far out of band.
    ops.clearLog();
    auto s2 = m.tick(1090.0e6, 2.4e6, 200,
                     ops.createFn(), ops.destroyFn(), ops.anchorFn());
    CHECK(s2.created == 0);
    CHECK(s2.destroyed == 1);
    CHECK(s2.anchored == 0);
    CHECK(ops.destroys.size() == 1);
    CHECK(!m.runtimeFor(id).in_band);
    CHECK(!m.runtimeFor(id).vfo_active);

    // Source moves back → re-create.
    ops.clearLog();
    auto s3 = m.tick(433.92e6, 2.4e6, 300,
                     ops.createFn(), ops.destroyFn(), ops.anchorFn());
    CHECK(s3.created == 1);
    CHECK(s3.anchored == 1);
    CHECK(s3.destroyed == 0);
    CHECK(m.runtimeFor(id).vfo_active);
}

static void test_anchor_reflects_offset() {
    HoldManager m;
    auto id = m.add(434.5e6, 12500.0, DecoderKind::Radio_NBFM, "", 1);
    CHECK(!id.empty());
    MockVfoOps ops;
    // Source centre 433.0 MHz → expected offset = +1.5 MHz.
    auto s = m.tick(433.0e6, 5.0e6, 100,
                    ops.createFn(), ops.destroyFn(), ops.anchorFn());
    CHECK(s.created == 1);
    CHECK(s.anchored == 1);
    CHECK(std::fabs(ops.anchors[0].offset - 1.5e6) < 1.0);
    CHECK(std::fabs(ops.anchors[0].bw - 12500.0) < 0.1);
}

static void test_in_band_boundary() {
    // Source @ 100 MHz, sr = 1 MHz → wideband window [99.5, 100.5] MHz.
    // Entry @ 100.49 MHz with bw = 20 kHz → half_bw 10 kHz → upper edge
    // at 100.500 MHz which is exactly the SDR upper edge. Boundary
    // case: should be in-band (<=, >=).
    CHECK( HoldManager::inBand(100.49e6, 20000.0, 100.0e6, 1.0e6));
    // 100.50 MHz with same bw → upper edge 100.510 MHz, out-of-band.
    CHECK(!HoldManager::inBand(100.50e6, 20000.0, 100.0e6, 1.0e6));
    // bw wider than sr can never be in-band.
    CHECK(!HoldManager::inBand(100.0e6, 2.0e6, 100.0e6, 1.0e6));
    // sr<=0 short-circuits to false.
    CHECK(!HoldManager::inBand(100.0e6, 12500.0, 100.0e6, 0.0));
    CHECK(!HoldManager::inBand(100.0e6, 12500.0, 100.0e6, -1.0));
    // Non-finite inputs short-circuit to false.
    CHECK(!HoldManager::inBand(std::nan(""), 12500.0, 100.0e6, 1.0e6));
    CHECK(!HoldManager::inBand(100.0e6, 12500.0, std::nan(""), 1.0e6));
}

static void test_disabled_entry_not_created() {
    HoldManager m;
    auto id = m.add(146.52e6, 12500.0, DecoderKind::Radio_NBFM, "2m", 1);
    CHECK(!id.empty());
    m.setEnabled(id, false);
    MockVfoOps ops;
    auto s = m.tick(146.52e6, 2.4e6, 100,
                    ops.createFn(), ops.destroyFn(), ops.anchorFn());
    CHECK(s.created == 0);
    CHECK(s.anchored == 0);
    CHECK(!m.runtimeFor(id).vfo_active);
    // in_band still reflects geometry — UI may want to show "in band
    // but paused" — but no VFO created.
    CHECK(m.runtimeFor(id).in_band);

    // Re-enable → next tick creates.
    m.setEnabled(id, true);
    ops.clearLog();
    auto s2 = m.tick(146.52e6, 2.4e6, 200,
                     ops.createFn(), ops.destroyFn(), ops.anchorFn());
    CHECK(s2.created == 1);
    CHECK(m.runtimeFor(id).vfo_active);

    // Disable an active entry → next tick destroys.
    m.setEnabled(id, false);
    ops.clearLog();
    auto s3 = m.tick(146.52e6, 2.4e6, 300,
                     ops.createFn(), ops.destroyFn(), ops.anchorFn());
    CHECK(s3.destroyed == 1);
    CHECK(!m.runtimeFor(id).vfo_active);
}

static void test_remove_triggers_gc_destroy() {
    HoldManager m;
    auto id = m.add(433.92e6, 12500.0, DecoderKind::Native_RTL433, "", 1);
    CHECK(!id.empty());
    MockVfoOps ops;
    m.tick(433.92e6, 2.4e6, 100,
           ops.createFn(), ops.destroyFn(), ops.anchorFn());
    CHECK(ops.creates.size() == 1);

    // Remove → next tick fires destroy via the GC pass.
    CHECK(m.remove(id));
    ops.clearLog();
    auto s = m.tick(433.92e6, 2.4e6, 200,
                    ops.createFn(), ops.destroyFn(), ops.anchorFn());
    CHECK(s.destroyed == 1);
    CHECK(ops.destroys.size() == 1);
    CHECK(ops.destroys[0].vfoName == "Predator H" + id);
    // Subsequent tick is a no-op (runtime fully GC'd).
    ops.clearLog();
    auto s2 = m.tick(433.92e6, 2.4e6, 300,
                     ops.createFn(), ops.destroyFn(), ops.anchorFn());
    CHECK(s2.created == 0);
    CHECK(s2.destroyed == 0);
    CHECK(s2.anchored == 0);
}

static void test_create_failure_retried_next_tick() {
    HoldManager m;
    auto id = m.add(433.92e6, 12500.0, DecoderKind::Native_RTL433, "", 1);
    CHECK(!id.empty());
    MockVfoOps ops;
    ops.createShouldFail = true;
    auto s1 = m.tick(433.92e6, 2.4e6, 100,
                     ops.createFn(), ops.destroyFn(), ops.anchorFn());
    CHECK(s1.created == 0);
    CHECK(!m.runtimeFor(id).vfo_active);
    // Recover.
    ops.createShouldFail = false;
    auto s2 = m.tick(433.92e6, 2.4e6, 200,
                     ops.createFn(), ops.destroyFn(), ops.anchorFn());
    CHECK(s2.created == 1);
    CHECK(m.runtimeFor(id).vfo_active);
}

static void test_decoder_kind_string_round_trip() {
    DecoderKind kinds[] = {
        DecoderKind::Radio_NBFM, DecoderKind::Radio_WBFM, DecoderKind::Radio_AM,
        DecoderKind::Radio_USB, DecoderKind::Radio_LSB, DecoderKind::Radio_DSB,
        DecoderKind::Radio_RAW,
        DecoderKind::Native_RTL433, DecoderKind::Native_ADSB,
        DecoderKind::Native_DSDFME_P25,
    };
    for (auto k : kinds) {
        const char* s = decoderKindToString(k);
        DecoderKind r = DecoderKind::Radio_AM;  // poison
        CHECK(decoderKindFromString(s, r));
        CHECK(r == k);
    }
    // Unknown string → false, output untouched.
    DecoderKind r = DecoderKind::Radio_LSB;
    CHECK(!decoderKindFromString("not_a_decoder", r));
    CHECK(r == DecoderKind::Radio_LSB);
}

static void test_json_round_trip() {
    HoldManager m;
    auto id1 = m.add(433.92e6, 12500.0, DecoderKind::Native_RTL433, "ISM", 1);
    auto id2 = m.add(146.52e6, 12500.0, DecoderKind::Radio_NBFM, "2m", 2);
    auto id3 = m.add(1090.0e6, 2.5e6,   DecoderKind::Native_ADSB, "ADS-B", 3);
    m.setEnabled(id2, false);
    CHECK(!id1.empty()); CHECK(!id2.empty()); CHECK(!id3.empty());

    nlohmann::json j = m.toJson();
    CHECK(j.is_array());
    CHECK(j.size() == 3);

    HoldManager n;
    n.fromJson(j);
    CHECK(n.size() == 3);
    CHECK(n.entries()[0].id == id1);
    CHECK(n.entries()[0].decoder == DecoderKind::Native_RTL433);
    CHECK(n.entries()[1].enabled == false);
    CHECK(n.entries()[2].decoder == DecoderKind::Native_ADSB);
    CHECK(std::fabs(n.entries()[2].bandwidth_hz - 2.5e6) < 1.0);
    // After fromJson, runtime is reset (no VFOs active).
    for (const auto& e : n.entries()) {
        CHECK(!n.runtimeFor(e.id).vfo_active);
    }
    // id_counter advances past the highest persisted id so a new
    // add() does not collide.
    auto id4 = n.add(915.0e6, 12500.0, DecoderKind::Radio_NBFM, "", 4);
    CHECK(!id4.empty());
    for (const auto& e : n.entries()) {
        if (e.id == id4) continue;
        CHECK(e.id != id4);
    }
}

static void test_json_skips_invalid_rows() {
    nlohmann::json j = nlohmann::json::array();
    // Valid row.
    j.push_back({{"id", "h1"}, {"label", "ok"}, {"frequency_hz", 100e6},
                 {"bandwidth_hz", 12500.0}, {"decoder", "radio_nbfm"},
                 {"created_ns", 1}, {"enabled", true}});
    // Invalid: zero frequency.
    j.push_back({{"id", "h2"}, {"frequency_hz", 0.0}, {"bandwidth_hz", 12500.0},
                 {"decoder", "radio_nbfm"}, {"enabled", true}});
    // Invalid: negative bandwidth.
    j.push_back({{"id", "h3"}, {"frequency_hz", 200e6}, {"bandwidth_hz", -1.0},
                 {"decoder", "radio_nbfm"}, {"enabled", true}});
    // Unknown decoder string falls back to Radio_NBFM (default).
    j.push_back({{"id", "h4"}, {"frequency_hz", 300e6}, {"bandwidth_hz", 12500.0},
                 {"decoder", "not_a_thing"}, {"enabled", true}});
    HoldManager m;
    m.fromJson(j);
    CHECK(m.size() == 2);
    CHECK(m.entries()[0].id == "h1");
    CHECK(m.entries()[1].id == "h4");
    CHECK(m.entries()[1].decoder == DecoderKind::Radio_NBFM);
}

static void test_clear_and_tick_destroys_all() {
    HoldManager m;
    auto id1 = m.add(433.92e6, 12500.0, DecoderKind::Native_RTL433, "", 1);
    auto id2 = m.add(434.10e6, 12500.0, DecoderKind::Radio_NBFM,    "", 2);
    CHECK(!id1.empty()); CHECK(!id2.empty());
    MockVfoOps ops;
    m.tick(434.0e6, 2.4e6, 100,
           ops.createFn(), ops.destroyFn(), ops.anchorFn());
    CHECK(ops.creates.size() == 2);
    m.clear();
    CHECK(m.size() == 0);
    ops.clearLog();
    auto s = m.tick(434.0e6, 2.4e6, 200,
                    ops.createFn(), ops.destroyFn(), ops.anchorFn());
    CHECK(s.destroyed == 2);
    CHECK(ops.destroys.size() == 2);
}

static void test_existscb_recovers_from_external_teardown() {
    // Regression for architect-found stuck-state bug: if a held VFO is
    // destroyed by something OTHER than the HoldManager (decoder module
    // reload, manual sigpath::vfoManager.deleteVFO from another path,
    // etc.), the next tick must notice via existsCb and rebuild rather
    // than sitting on stale rt.vfo_active==true forever.
    HoldManager m;
    auto id = m.add(433.92e6, 12500.0, DecoderKind::Native_RTL433, "", 1);
    CHECK(!id.empty());
    MockVfoOps ops;
    // Mock "live VFO set" that existsCb consults.
    std::vector<std::string> liveVfos;
    auto existsCb = [&](const std::string& v) {
        return std::find(liveVfos.begin(), liveVfos.end(), v) != liveVfos.end();
    };
    auto createWithRegister = [&](const HoldEntry& e, const std::string& v, double off) {
        liveVfos.push_back(v);
        return ops.createFn()(e, v, off);
    };
    auto destroyWithRegister = [&](const std::string& v) {
        liveVfos.erase(std::remove(liveVfos.begin(), liveVfos.end(), v), liveVfos.end());
        ops.destroyFn()(v);
    };

    // Create normally.
    auto s1 = m.tick(433.92e6, 2.4e6, 100,
                     createWithRegister, destroyWithRegister, ops.anchorFn(), existsCb);
    CHECK(s1.created == 1);
    CHECK(m.runtimeFor(id).vfo_active);
    CHECK(liveVfos.size() == 1);

    // External teardown — bypass the manager entirely.
    liveVfos.clear();
    ops.clearLog();

    // Next tick must notice (via existsCb), drop stale state, rebuild.
    auto s2 = m.tick(433.92e6, 2.4e6, 200,
                     createWithRegister, destroyWithRegister, ops.anchorFn(), existsCb);
    CHECK(s2.created == 1);
    CHECK(s2.destroyed == 0);  // we never tear down what's already gone
    CHECK(m.runtimeFor(id).vfo_active);
    CHECK(liveVfos.size() == 1);

    // Without existsCb supplied, the manager keeps the older behaviour
    // (callers that own the VFO lifecycle exclusively get the same code
    // path as before this fix).
    liveVfos.clear();
    ops.clearLog();
    auto s3 = m.tick(433.92e6, 2.4e6, 300,
                     createWithRegister, destroyWithRegister, ops.anchorFn());
    CHECK(s3.created == 0);            // no recovery without existsCb
    CHECK(m.runtimeFor(id).vfo_active); // still stuck (documented behaviour)
}

static void test_null_callbacks_are_safe() {
    // Wire-up may pass null callbacks during shutdown — tick must
    // not segfault and must keep its book-keeping consistent.
    HoldManager m;
    auto id = m.add(433.92e6, 12500.0, DecoderKind::Native_RTL433, "", 1);
    CHECK(!id.empty());
    auto s = m.tick(433.92e6, 2.4e6, 100,
                    HoldManager::CreateFn{},
                    HoldManager::DestroyFn{},
                    HoldManager::AnchorFn{});
    // Without a createCb, no creation happens — but no crash.
    CHECK(s.created == 0);
    CHECK(!m.runtimeFor(id).vfo_active);
}

int main() {
    test_add_and_size_limits();
    test_lifecycle_in_and_out_of_band();
    test_anchor_reflects_offset();
    test_in_band_boundary();
    test_disabled_entry_not_created();
    test_remove_triggers_gc_destroy();
    test_create_failure_retried_next_tick();
    test_decoder_kind_string_round_trip();
    test_json_round_trip();
    test_json_skips_invalid_rows();
    test_clear_and_tick_destroys_all();
    test_existscb_recovers_from_external_teardown();
    test_null_callbacks_are_safe();
    // ----- Architect-flagged: bandwidth override is applied per-frame
    // by anchorCb and inBand, not just at create time.
    {
        using namespace predator::hold;
        HoldManager hm;
        std::string id = hm.add(433.92e6, 12500.0, DecoderKind::Native_RTL433,
                                "ovr", 1);
        CHECK(!id.empty());
        std::string lastVfo;
        double      lastBw = -1.0;
        auto createCb = [&](const HoldEntry&, const std::string& v, double) {
            lastVfo = v; return true;
        };
        auto destroyCb = [&](const std::string&) {};
        auto anchorCb  = [&](const std::string&, double, double bw) { lastBw = bw; };
        auto bwOver    = [](const HoldEntry& he) -> double {
            return (he.decoder == DecoderKind::Native_RTL433) ? 250000.0 : 0.0;
        };
        // Tick 1: create + anchor.  Anchor must see effective bw 250k.
        hm.tick(433.92e6, 2.4e6, 1, createCb, destroyCb, anchorCb,
                HoldManager::ExistsFn{}, bwOver);
        CHECK(lastBw == 250000.0);
        // Tick 2: anchor again. Must STILL be 250k (drift check).
        lastBw = -1.0;
        hm.tick(433.92e6, 2.4e6, 2, createCb, destroyCb, anchorCb,
                HoldManager::ExistsFn{}, bwOver);
        CHECK(lastBw == 250000.0);
        // Without the override, anchor would see 12500.
        lastBw = -1.0;
        hm.tick(433.92e6, 2.4e6, 3, createCb, destroyCb, anchorCb);
        CHECK(lastBw == 12500.0);
    }
    std::printf("hold_manager_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
