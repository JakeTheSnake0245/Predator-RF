// Standalone unit tests for predator::hold::HoldDecoderBinder.
//
// Build:
//   g++ -std=c++17 -O2 -Icore/src tests/hold_decoder_binder_test.cpp \
//       -o /tmp/hdbt && /tmp/hdbt
//
// The binder is header-only and sigpath-free; everything is mocked
// through std::function callbacks.

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "predator/hold_decoder_binder.h"
#include "predator/hold_manager.h"

using predator::hold::DecoderKind;
using predator::hold::HoldDecoderBinder;
using predator::hold::HoldEntry;
using predator::hold::HoldManager;
using predator::hold::decoderModuleName;
using predator::hold::requiredVfoBandwidth;

static int g_pass = 0;
static int g_fail = 0;
#define CHECK(cond) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

// Mock instance manager that mirrors moduleManager.createInstance/deleteInstance
// for accounting purposes only.
struct MockInstanceMgr {
    std::vector<std::string>                       liveInstances;
    std::vector<std::pair<std::string,std::string>> createLog;  // (instName, modName)
    std::vector<std::string>                       destroyLog;
    bool                                           failNextCreate = false;

    HoldDecoderBinder::CreateInstanceFn createFn() {
        return [this](const std::string& instName,
                      const std::string& modName,
                      const std::string& /*vfoName*/) -> bool {
            if (failNextCreate) { failNextCreate = false; return false; }
            createLog.push_back({instName, modName});
            liveInstances.push_back(instName);
            return true;
        };
    }
    HoldDecoderBinder::DestroyInstanceFn destroyFn() {
        return [this](const std::string& instName) {
            destroyLog.push_back(instName);
            liveInstances.erase(std::remove(liveInstances.begin(), liveInstances.end(), instName),
                                liveInstances.end());
        };
    }
};

HoldEntry makeEntry(const std::string& id, double freq, DecoderKind dec, bool enabled = true) {
    HoldEntry e;
    e.id            = id;
    e.frequency_hz  = freq;
    e.bandwidth_hz  = 250000.0;  // RTL433-friendly width by default
    e.decoder       = dec;
    e.enabled       = enabled;
    e.created_ns    = 1;
    return e;
}

}  // namespace

// -----------------------------------------------------------------------------

static void test_decoder_module_name_mapping() {
    // RTL433 is the only kind that gets auto-activated in the #5 cut.
    CHECK(std::string(decoderModuleName(DecoderKind::Native_RTL433)) == "rtl433_decoder");
    // Everything else is "" — deferred to #5.5 / #5.6.
    CHECK(std::string(decoderModuleName(DecoderKind::Native_DSDFME_P25)) == "");
    CHECK(std::string(decoderModuleName(DecoderKind::Native_ADSB)) == "");
    CHECK(std::string(decoderModuleName(DecoderKind::Radio_NBFM)) == "");
    CHECK(std::string(decoderModuleName(DecoderKind::Radio_WBFM)) == "");
    CHECK(std::string(decoderModuleName(DecoderKind::Radio_AM)) == "");
}

static void test_required_bandwidth_for_decoder() {
    CHECK(requiredVfoBandwidth(DecoderKind::Native_RTL433) == 250000.0);
    CHECK(requiredVfoBandwidth(DecoderKind::Radio_NBFM)    == 0.0);
    CHECK(requiredVfoBandwidth(DecoderKind::Native_ADSB)   == 0.0);
}

static void test_naming_convention() {
    auto e = makeEntry("h7", 433.92e6, DecoderKind::Native_RTL433);
    CHECK(HoldDecoderBinder::vfoNameFor(e)      == "Predator Hh7");
    CHECK(HoldDecoderBinder::instanceNameFor(e) == "predator_hold_h7");
}

static void test_spawn_when_vfo_present() {
    HoldDecoderBinder b;
    MockInstanceMgr m;
    auto e = makeEntry("h1", 433.92e6, DecoderKind::Native_RTL433);
    std::vector<HoldEntry> entries{e};
    // VFO present → spawn.
    auto exists = [](const std::string& v) { return v == "Predator Hh1"; };
    auto pre  = b.preTick(entries, 433.92e6, 2.4e6, m.destroyFn());
    auto post = b.postTick(entries, exists, m.createFn());
    CHECK(pre.torn_down == 0);
    CHECK(post.spawned  == 1);
    CHECK(post.deferred == 0);
    CHECK(b.isActive("h1"));
    CHECK(b.instanceNameOf("h1") == "predator_hold_h1");
    CHECK(m.createLog.size() == 1);
    CHECK(m.createLog[0].second == "rtl433_decoder");
}

static void test_spawn_deferred_when_vfo_missing() {
    HoldDecoderBinder b;
    MockInstanceMgr m;
    auto e = makeEntry("h2", 433.92e6, DecoderKind::Native_RTL433);
    std::vector<HoldEntry> entries{e};
    // VFO absent (HoldManager will create it but not yet) → deferred.
    auto noExists = [](const std::string&) { return false; };
    auto post = b.postTick(entries, noExists, m.createFn());
    CHECK(post.spawned  == 0);
    CHECK(post.deferred == 1);
    CHECK(!b.isActive("h2"));
    CHECK(m.createLog.empty());
    // Next frame, VFO appears → spawn.
    auto exists = [](const std::string&) { return true; };
    auto post2 = b.postTick(entries, exists, m.createFn());
    CHECK(post2.spawned == 1);
    CHECK(b.isActive("h2"));
}

static void test_no_spawn_for_unsupported_decoder() {
    HoldDecoderBinder b;
    MockInstanceMgr m;
    auto e = makeEntry("h3", 162.55e6, DecoderKind::Radio_NBFM);
    std::vector<HoldEntry> entries{e};
    auto exists = [](const std::string&) { return true; };
    auto post = b.postTick(entries, exists, m.createFn());
    CHECK(post.spawned == 0);
    CHECK(post.deferred == 0);
    CHECK(!b.isActive("h3"));
}

static void test_teardown_when_entry_removed() {
    HoldDecoderBinder b;
    MockInstanceMgr m;
    auto e = makeEntry("h4", 433.92e6, DecoderKind::Native_RTL433);
    std::vector<HoldEntry> entries{e};
    auto exists = [](const std::string&) { return true; };
    b.postTick(entries, exists, m.createFn());
    CHECK(b.isActive("h4"));
    // Entry removed → preTick tears down.
    std::vector<HoldEntry> empty;
    auto pre = b.preTick(empty, 433.92e6, 2.4e6, m.destroyFn());
    CHECK(pre.torn_down == 1);
    CHECK(!b.isActive("h4"));
    CHECK(m.destroyLog.size() == 1);
    CHECK(m.destroyLog[0] == "predator_hold_h4");
}

static void test_teardown_when_entry_disabled() {
    HoldDecoderBinder b;
    MockInstanceMgr m;
    auto e = makeEntry("h5", 433.92e6, DecoderKind::Native_RTL433, /*enabled*/true);
    std::vector<HoldEntry> entries{e};
    auto exists = [](const std::string&) { return true; };
    b.postTick(entries, exists, m.createFn());
    CHECK(b.isActive("h5"));
    entries[0].enabled = false;
    auto pre = b.preTick(entries, 433.92e6, 2.4e6, m.destroyFn());
    CHECK(pre.torn_down == 1);
    CHECK(!b.isActive("h5"));
}

static void test_teardown_when_decoder_changed() {
    HoldDecoderBinder b;
    MockInstanceMgr m;
    auto e = makeEntry("h6", 433.92e6, DecoderKind::Native_RTL433);
    std::vector<HoldEntry> entries{e};
    auto exists = [](const std::string&) { return true; };
    b.postTick(entries, exists, m.createFn());
    CHECK(b.isActive("h6"));
    entries[0].decoder = DecoderKind::Radio_NBFM;
    auto pre = b.preTick(entries, 433.92e6, 2.4e6, m.destroyFn());
    CHECK(pre.torn_down == 1);
    CHECK(!b.isActive("h6"));
    // postTick now sees an unsupported decoder → no respawn.
    auto post = b.postTick(entries, exists, m.createFn());
    CHECK(post.spawned == 0);
}

static void test_teardown_when_out_of_band() {
    HoldDecoderBinder b;
    MockInstanceMgr m;
    auto e = makeEntry("h7", 433.92e6, DecoderKind::Native_RTL433);
    std::vector<HoldEntry> entries{e};
    auto exists = [](const std::string&) { return true; };
    // In-band first.
    b.postTick(entries, exists, m.createFn());
    CHECK(b.isActive("h7"));
    // Source retunes to 868 MHz → 433 MHz now outside the spectrum window.
    auto pre = b.preTick(entries, 868.0e6, 2.4e6, m.destroyFn());
    CHECK(pre.torn_down == 1);
    CHECK(!b.isActive("h7"));
    // Source retunes back → respawn.
    b.postTick(entries, exists, m.createFn());
    CHECK(b.isActive("h7"));
}

static void test_create_failure_is_retried_next_tick() {
    HoldDecoderBinder b;
    MockInstanceMgr m;
    auto e = makeEntry("h8", 433.92e6, DecoderKind::Native_RTL433);
    std::vector<HoldEntry> entries{e};
    auto exists = [](const std::string&) { return true; };
    m.failNextCreate = true;
    auto post = b.postTick(entries, exists, m.createFn());
    CHECK(post.spawned  == 0);
    CHECK(post.deferred == 1);
    CHECK(!b.isActive("h8"));
    // Next frame the failure flag is cleared by the mock — should spawn now.
    auto post2 = b.postTick(entries, exists, m.createFn());
    CHECK(post2.spawned == 1);
    CHECK(b.isActive("h8"));
}

static void test_clear_tears_down_all() {
    HoldDecoderBinder b;
    MockInstanceMgr m;
    std::vector<HoldEntry> entries{
        makeEntry("h9",  433.92e6, DecoderKind::Native_RTL433),
        makeEntry("h10", 433.92e6, DecoderKind::Native_RTL433),
    };
    auto exists = [](const std::string&) { return true; };
    b.postTick(entries, exists, m.createFn());
    CHECK(b.activeCount() == 2);
    b.clear(m.destroyFn());
    CHECK(b.activeCount() == 0);
    CHECK(m.destroyLog.size() == 2);
}

static void test_null_callbacks_are_safe() {
    // Pre/postTick with null callbacks must not crash; useful during
    // shutdown when the wire-up may have already torn down the
    // ModuleManager glue.
    HoldDecoderBinder b;
    auto e = makeEntry("h11", 433.92e6, DecoderKind::Native_RTL433);
    std::vector<HoldEntry> entries{e};
    auto exists = [](const std::string&) { return true; };
    auto post = b.postTick(entries, exists, HoldDecoderBinder::CreateInstanceFn{});
    CHECK(post.spawned  == 0);
    CHECK(post.deferred == 1);  // create returned false → deferred
    auto pre = b.preTick({}, 433.92e6, 2.4e6, HoldDecoderBinder::DestroyInstanceFn{});
    CHECK(pre.torn_down == 0);
}

static void test_no_double_spawn() {
    HoldDecoderBinder b;
    MockInstanceMgr m;
    auto e = makeEntry("h12", 433.92e6, DecoderKind::Native_RTL433);
    std::vector<HoldEntry> entries{e};
    auto exists = [](const std::string&) { return true; };
    b.postTick(entries, exists, m.createFn());
    auto post2 = b.postTick(entries, exists, m.createFn());
    auto post3 = b.postTick(entries, exists, m.createFn());
    CHECK(post2.spawned == 0);
    CHECK(post3.spawned == 0);
    CHECK(m.createLog.size() == 1);
}

// Architect-flagged: preTick must use the SAME effective bandwidth math
// HoldManager will apply this frame.  An RTL433 entry with a narrow UI
// bandwidth must NOT trip the out-of-band branch when the manager will
// keep the VFO alive at the wider 250 kHz effective bw.
static void test_preTick_uses_effective_bandwidth_for_inband() {
    HoldDecoderBinder b;
    MockInstanceMgr m;
    auto e = makeEntry("h13", 433.92e6, DecoderKind::Native_RTL433);
    e.bandwidth_hz = 12500.0;  // Narrow UI bw — but RTL433 forces 250k.
    std::vector<HoldEntry> entries{e};
    auto exists = [](const std::string&) { return true; };
    b.postTick(entries, exists, m.createFn());
    CHECK(b.isActive("h13"));
    // Source center 200 kHz away from held freq, sample rate 1 MHz.
    // With UI bw=12.5k → naive inBand says in-band (passband fits).
    // With eff bw=250k → still in-band (200k+125k=325k < 500k half-sr).
    auto pre1 = b.preTick(entries, 433.72e6, 1.0e6, m.destroyFn());
    CHECK(pre1.torn_down == 0);
    CHECK(b.isActive("h13"));
    // Now retune so 250k bw goes out-of-band but 12.5k bw would not.
    // freq=433.92e6, center=434.30e6 → offset 380k.
    // 12.5k bw: 380k + 6.25k = 386.25k < 500k → in-band.
    // 250k bw: 380k + 125k  = 505k  > 500k → OUT of band.
    // Binder must agree with HoldManager (effective bw) and tear down.
    auto pre2 = b.preTick(entries, 434.30e6, 1.0e6, m.destroyFn());
    CHECK(pre2.torn_down == 1);
    CHECK(!b.isActive("h13"));
}

// Architect-flagged: external instance deletion (module reload, manual
// operator delete) must trigger respawn, not stick forever like the
// pre-existsCb HoldManager bug.  Mirrors HoldManager's existsCb fix.
static void test_external_instance_delete_triggers_respawn() {
    HoldDecoderBinder b;
    MockInstanceMgr m;
    auto e = makeEntry("h14", 433.92e6, DecoderKind::Native_RTL433);
    std::vector<HoldEntry> entries{e};
    auto exists = [](const std::string&) { return true; };
    b.postTick(entries, exists, m.createFn());
    CHECK(b.isActive("h14"));
    CHECK(m.liveInstances.size() == 1);
    // Simulate external delete (e.g., operator deleted from source-side
    // menu) — instance is gone but binder doesn't know.
    m.liveInstances.clear();
    auto instExists = [&](const std::string& n) {
        return std::find(m.liveInstances.begin(), m.liveInstances.end(), n)
               != m.liveInstances.end();
    };
    // preTick with the existsCb should silently drop the stale
    // active_ entry — no destroyCb fires (instance is already gone).
    auto pre = b.preTick(entries, 433.92e6, 2.4e6, m.destroyFn(), instExists);
    CHECK(pre.torn_down == 0);  // didn't tear down — already gone
    CHECK(!b.isActive("h14"));
    CHECK(m.destroyLog.empty());
    // postTick respawns under the same name.
    auto post = b.postTick(entries, exists, m.createFn());
    CHECK(post.spawned == 1);
    CHECK(b.isActive("h14"));
    CHECK(m.liveInstances.size() == 1);
}

// Without instanceExistsCb, pre-existing behaviour is preserved (binder
// trusts active_ membership).  Documents the "stuck" mode for callers
// that don't supply the existsCb.
// Architect-flagged: false-negative instanceExistsCb in preTick (says
// gone for one frame while instance is actually live) must self-heal
// via the adoption path — without it, createInstance keeps failing
// with "already exists" and the binder loops forever in deferred state.
static void test_postTick_adopts_on_create_collision() {
    HoldDecoderBinder b;
    MockInstanceMgr m;
    auto e = makeEntry("h_adopt", 433.92e6, DecoderKind::Native_RTL433);
    std::vector<HoldEntry> entries{e};
    auto vfoExists = [](const std::string&) { return true; };
    // Simulate a false-negative: liveInstances says gone, but we'll
    // pretend the OS-level instance is actually still present by
    // having the create callback fail (collision) AND a separate
    // instanceExistsCb report "alive".
    bool osInstanceAlive = true;
    auto failingCreate = [&](const std::string&, const std::string&,
                             const std::string&) -> bool {
        return false;  // moduleManager says "name already in use"
    };
    auto instExists = [&](const std::string&) { return osInstanceAlive; };
    auto post = b.postTick(entries, vfoExists, failingCreate, instExists);
    // Adoption path: spawned counter incremented, active_ tracks it.
    CHECK(post.spawned  == 1);
    CHECK(post.deferred == 0);
    CHECK(b.isActive("h_adopt"));
    // If the instance is genuinely gone AND create fails, the binder
    // must NOT adopt — it stays deferred and retries next frame.
    HoldDecoderBinder b2;
    osInstanceAlive = false;
    auto post2 = b2.postTick(entries, vfoExists, failingCreate, instExists);
    CHECK(post2.spawned  == 0);
    CHECK(post2.deferred == 1);
    CHECK(!b2.isActive("h_adopt"));
}

static void test_no_instanceExistsCb_preserves_stuck_behaviour() {
    HoldDecoderBinder b;
    MockInstanceMgr m;
    auto e = makeEntry("h15", 433.92e6, DecoderKind::Native_RTL433);
    std::vector<HoldEntry> entries{e};
    auto exists = [](const std::string&) { return true; };
    b.postTick(entries, exists, m.createFn());
    CHECK(b.isActive("h15"));
    // Simulate external delete...
    m.liveInstances.clear();
    // ...without supplying instanceExistsCb. Binder stays stuck.
    auto pre = b.preTick(entries, 433.92e6, 2.4e6, m.destroyFn());
    CHECK(pre.torn_down == 0);
    CHECK(b.isActive("h15"));   // stuck
    auto post = b.postTick(entries, exists, m.createFn());
    CHECK(post.spawned == 0);   // no respawn
}

// Architect-flagged safety bug: false-negative instanceExistsCb in
// preTick must NOT bypass destroyCb when the entry ALSO wants to be
// torn down.  Three sub-cases pin the destroy path: entry-removed,
// entry-disabled, out-of-band.  Failing this would re-introduce the
// exact dsp::sink::Handler-vs-freed-stream race #5 was built to fix.
static void test_preTick_destroyCb_runs_even_when_existsCb_false_negative() {
    // (a) Entry removed + false-negative exists.
    {
        HoldDecoderBinder b;
        MockInstanceMgr m;
        auto e = makeEntry("a1", 433.92e6, DecoderKind::Native_RTL433);
        std::vector<HoldEntry> entries{e};
        auto vfoExists = [](const std::string&) { return true; };
        b.postTick(entries, vfoExists, m.createFn());
        CHECK(b.isActive("a1"));
        // Entry removed; instanceExistsCb spuriously returns false.
        std::vector<HoldEntry> empty;
        auto falseExists = [](const std::string&) { return false; };
        auto pre = b.preTick(empty, 433.92e6, 2.4e6, m.destroyFn(), falseExists);
        CHECK(pre.torn_down == 1);              // best-effort destroy fired
        CHECK(m.destroyLog.size() == 1);
        CHECK(!b.isActive("a1"));
    }
    // (b) Entry disabled + false-negative exists.
    {
        HoldDecoderBinder b;
        MockInstanceMgr m;
        auto e = makeEntry("a2", 433.92e6, DecoderKind::Native_RTL433);
        std::vector<HoldEntry> entries{e};
        auto vfoExists = [](const std::string&) { return true; };
        b.postTick(entries, vfoExists, m.createFn());
        entries[0].enabled = false;
        auto falseExists = [](const std::string&) { return false; };
        auto pre = b.preTick(entries, 433.92e6, 2.4e6, m.destroyFn(), falseExists);
        CHECK(pre.torn_down == 1);
        CHECK(m.destroyLog.size() == 1);
        CHECK(!b.isActive("a2"));
    }
    // (c) Out-of-band + false-negative exists.
    {
        HoldDecoderBinder b;
        MockInstanceMgr m;
        auto e = makeEntry("a3", 433.92e6, DecoderKind::Native_RTL433);
        std::vector<HoldEntry> entries{e};
        auto vfoExists = [](const std::string&) { return true; };
        b.postTick(entries, vfoExists, m.createFn());
        auto falseExists = [](const std::string&) { return false; };
        // Retune source far away → out-of-band.
        auto pre = b.preTick(entries, 868.0e6, 2.4e6, m.destroyFn(), falseExists);
        CHECK(pre.torn_down == 1);
        CHECK(m.destroyLog.size() == 1);
        CHECK(!b.isActive("a3"));
    }
}

static void test_inband_math_mirrors_holdmanager() {
    // Sanity check: the binder's preTick uses HoldManager::inBand. Pin
    // the boundary cases to catch any future signature drift.
    CHECK( HoldManager::inBand(433.92e6, 250000.0, 433.92e6, 2.4e6));
    CHECK( HoldManager::inBand(433.92e6, 250000.0, 434.0e6,  2.4e6));
    CHECK(!HoldManager::inBand(868.0e6,  250000.0, 433.92e6, 2.4e6));
    // Edge: held bw eats the entire SDR window → out-of-band.
    CHECK(!HoldManager::inBand(433.92e6, 3.0e6,    433.92e6, 2.4e6));
}

int main() {
    test_decoder_module_name_mapping();
    test_required_bandwidth_for_decoder();
    test_naming_convention();
    test_spawn_when_vfo_present();
    test_spawn_deferred_when_vfo_missing();
    test_no_spawn_for_unsupported_decoder();
    test_teardown_when_entry_removed();
    test_teardown_when_entry_disabled();
    test_teardown_when_decoder_changed();
    test_teardown_when_out_of_band();
    test_create_failure_is_retried_next_tick();
    test_clear_tears_down_all();
    test_null_callbacks_are_safe();
    test_no_double_spawn();
    test_preTick_uses_effective_bandwidth_for_inband();
    test_external_instance_delete_triggers_respawn();
    test_postTick_adopts_on_create_collision();
    test_no_instanceExistsCb_preserves_stuck_behaviour();
    test_preTick_destroyCb_runs_even_when_existsCb_false_negative();
    test_inband_math_mirrors_holdmanager();
    std::printf("hold_decoder_binder_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
