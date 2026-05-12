// SPDX-License-Identifier: GPL-3.0-only
//
// predator::hold::HoldDecoderBinder — auto-spawn decoder ModuleManager
// instances for held entries.
//
// Why this exists (roadmap #5)
// ----------------------------
// HoldManager (predator/hold_manager.h) owns persistent VFOs for held
// frequencies but does NOT activate any decoder against them.  After #4
// shipped, the operator still had to manually drag a decoder onto the
// "Predator H<id>" VFO from the source-side menu.  Roadmap #5 closes
// that gap for native_rtl433 (the most useful "set and forget" case for
// pinned ISM frequencies).  DSDFME / radio decoder activation are
// explicitly deferred to follow-on items #5.5 / #5.6 — the binder maps
// those decoder kinds to "" (no module) and ignores them.
//
// Lifecycle contract
// ------------------
// Binder.preTick(entries, sourceCenter, sampleRate, destroyInstanceCb):
//     For each currently-active instance, tear down if:
//       (a) the entry is gone, OR
//       (b) the entry is disabled, OR
//       (c) the entry's decoder no longer maps to a module, OR
//       (d) the entry's decoder kind changed, OR
//       (e) the entry will be out-of-band after HoldManager.tick this
//           frame (uses HoldManager::inBand math directly so we don't
//           leave a dsp::sink::Handler reading from a stream that
//           HoldManager is about to free in the same frame).
//
// Binder.postTick(entries, vfoExistsCb, createInstanceCb):
//     For each entry that wants an instance and doesn't have one,
//     spawn one IF its VFO already exists.  If the VFO doesn't exist
//     yet (HoldManager will create it this frame, or it's out-of-band
//     and was just torn down), the spawn is deferred to the next frame.
//
// Wire-up call order in main_window.cpp:
//     binder.preTick(entries, cf, sr, destroyCb);   //  --- BEFORE
//     holdManager.tick(cf, sr, ...);                //  --- (creates/destroys VFOs)
//     binder.postTick(entries, vfoExistsCb, createCb);  //  --- AFTER
//
// Two-phase split is load-bearing: tearing down before the VFO is
// destroyed keeps the dsp handler attached to a live stream until
// stopPipeline() runs; spawning after the VFO is created means the
// decoder module's ctor finds its bound VFO immediately.
//
// Test surface
// ------------
// Header-only and dependency-free (just stdlib + hold_manager.h for the
// HoldEntry struct & inBand helper).  All sigpath / ModuleManager calls
// flow through std::function callbacks so tests/hold_decoder_binder_test.cpp
// can run from g++ on Replit without linking against sdrpp_core.

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "hold_manager.h"

namespace predator {
namespace hold {

// Maps a held-entry decoder choice to a SDRPP module name registered
// with ModuleManager.  Returns "" for decoder kinds we don't yet know
// how to auto-activate (Radio_*, Native_ADSB, Native_DSDFME_P25 — the
// operator still drives those manually via the source-side menu;
// roadmap #5.5 / #5.6 will extend this map).
inline const char* decoderModuleName(DecoderKind k) {
    switch (k) {
        case DecoderKind::Native_RTL433: return "rtl433_decoder";
        // Deferred (#5.5 DSDFME, #5.6 Radio):
        case DecoderKind::Native_DSDFME_P25:
        case DecoderKind::Native_ADSB:
        case DecoderKind::Radio_NBFM:
        case DecoderKind::Radio_WBFM:
        case DecoderKind::Radio_AM:
        case DecoderKind::Radio_USB:
        case DecoderKind::Radio_LSB:
        case DecoderKind::Radio_DSB:
        case DecoderKind::Radio_RAW:
        default:
            return "";
    }
}

// Some decoders need a specific input sample rate / VFO bandwidth. The
// wire-up's HoldManager createCb consults this and overrides the held
// entry's nominal bandwidth so the spawned decoder finds a VFO it can
// actually consume.  Returns 0 = no override.
inline double requiredVfoBandwidth(DecoderKind k) {
    switch (k) {
        case DecoderKind::Native_RTL433: return 250000.0;  // RTL433_INPUT_RATE
        default: return 0.0;
    }
}

class HoldDecoderBinder {
public:
    using DestroyInstanceFn = std::function<void(const std::string& instanceName)>;
    using ExistsVfoFn       = std::function<bool(const std::string& vfoName)>;
    using ExistsInstanceFn  = std::function<bool(const std::string& instanceName)>;
    using CreateInstanceFn  = std::function<bool(const std::string& instanceName,
                                                 const std::string& moduleName,
                                                 const std::string& vfoName)>;

    struct Stats {
        int spawned       = 0;
        int torn_down     = 0;
        int deferred      = 0;  // wanted to spawn but VFO not yet present
    };

    HoldDecoderBinder() = default;

    // VFO name convention; mirrors HoldManager wire-up in main_window.cpp.
    static std::string vfoNameFor(const HoldEntry& e) {
        return std::string("Predator H") + e.id;
    }
    // Module instance name convention.  Stable, predictable, and never
    // collides with operator-named instances (no operator would name
    // their RTL433 instance "predator_hold_h7").
    static std::string instanceNameFor(const HoldEntry& e) {
        return std::string("predator_hold_") + e.id;
    }

    // Tear down stale / out-of-band instances BEFORE HoldManager.tick.
    // entries: caller passes HoldManager.entries().
    // sourceCenterHz / sampleRateHz: same values that will go into
    //     HoldManager.tick this frame, so our in-band predictions match.
    // destroyCb: called for each instance that needs to die.  May be
    //     nullable; if null, internal state is updated but no external
    //     teardown happens (test-only).
    // instanceExistsCb (optional): if supplied, the binder uses it to
    // reality-check each tracked instance against the live module
    // manager.  Without it, an external delete (module reload, manual
    // operator delete via the source-side menu) leaves active_ stuck
    // believing the instance still exists, and postTick never respawns.
    // Mirrors the existsCb fix HoldManager has for VFOs.
    Stats preTick(const std::vector<HoldEntry>& entries,
                  double sourceCenterHz, double sampleRateHz,
                  const DestroyInstanceFn& destroyCb,
                  const ExistsInstanceFn& instanceExistsCb = ExistsInstanceFn{}) {
        Stats s{};
        // Build id -> entry view so we can short-circuit.
        std::unordered_map<std::string, const HoldEntry*> live;
        live.reserve(entries.size());
        for (const auto& e : entries) live[e.id] = &e;

        // ORDERING NOTE (architect-flagged safety bug):
        // The drop decision must run BEFORE the instanceExistsCb short-
        // circuit.  If we let a false-negative instanceExistsCb erase
        // active_ without firing destroyCb when the entry ALSO wants to
        // be torn down (removed / disabled / out-of-band), HoldManager
        // would destroy the bound VFO while a live dsp::sink::Handler
        // is still attached — the exact race roadmap #5 was designed
        // to prevent.  So: compute drop first; if drop → call destroyCb
        // unconditionally (it must be idempotent).  Only when the entry
        // wants to KEEP the instance do we use instanceExistsCb to
        // detect external deletion and let postTick respawn.
        std::vector<std::string> idsToDrop;
        std::vector<std::string> idsExternallyGone;
        for (const auto& kv : active_) {
            const std::string& id = kv.first;
            const Active& a = kv.second;
            auto it = live.find(id);
            const HoldEntry* e = (it == live.end()) ? nullptr : it->second;
            bool drop = false;
            if (!e) {
                drop = true;  // entry removed
            } else if (!e->enabled) {
                drop = true;  // entry paused
            } else if (a.decoder != e->decoder) {
                drop = true;  // operator changed decoder kind
            } else if (decoderModuleName(e->decoder)[0] == '\0') {
                drop = true;  // decoder no longer auto-activatable
            } else {
                // Use the same effective bandwidth math the wire-up
                // passes to HoldManager.tick — RTL433-bound entries
                // sit at 250 kHz regardless of e.bandwidth_hz, so a
                // narrow UI bandwidth must NOT make us predict
                // out-of-band when the manager will keep the VFO
                // alive at the wider effective bw.
                double effBw = e->bandwidth_hz;
                double reqBw = requiredVfoBandwidth(e->decoder);
                if (reqBw > 0.0) effBw = reqBw;
                if (!HoldManager::inBand(e->frequency_hz, effBw,
                                         sourceCenterHz, sampleRateHz)) {
                    drop = true;  // VFO will be torn down this frame
                }
            }
            if (drop) {
                idsToDrop.push_back(id);
            } else if (instanceExistsCb && !instanceExistsCb(a.instance_name)) {
                // Entry wants to keep its instance but the module was
                // deleted out from under us — silent drop so postTick
                // can respawn this frame.
                idsExternallyGone.push_back(id);
            }
        }
        for (const auto& id : idsToDrop) {
            const std::string instName = active_[id].instance_name;
            // Best-effort destroy.  destroyCb must be idempotent — in
            // production moduleManager.deleteInstance logs an error and
            // returns -1 if the name is already gone, but does not
            // crash, which is exactly the contract we need here.
            if (destroyCb) destroyCb(instName);
            active_.erase(id);
            ++s.torn_down;
        }
        for (const auto& id : idsExternallyGone) {
            // We rely on the wire-up's createInstCb to overwrite the
            // (now-stale) binding via setBoundVfoFor before the next
            // createInstance reads it.  We do NOT clearBoundVfoFor
            // here because doing so without holding the same lock
            // ordering as the wire-up could race a concurrent respawn
            // attempt; the overwrite-on-respawn path is sufficient.
            active_.erase(id);
            // No torn_down++ — nothing was actually torn down by us.
        }
        return s;
    }

    // Spawn instances for entries that want decoders AFTER HoldManager.tick.
    //
    // instanceExistsCb (optional): adoption path for false-negative
    // exists scenarios.  If create fails AND the instance actually
    // exists (e.g., a one-frame stale exists check in preTick spuriously
    // dropped a still-live instance, then createInstance refused with
    // "already exists"), the binder ADOPTS the live instance into
    // active_ instead of looping forever on a deferred-spawn.  Without
    // this hook the binder would never recover from the false-negative
    // because every subsequent create would also fail with the same
    // collision.
    Stats postTick(const std::vector<HoldEntry>& entries,
                   const ExistsVfoFn& vfoExistsCb,
                   const CreateInstanceFn& createCb,
                   const ExistsInstanceFn& instanceExistsCb = ExistsInstanceFn{}) {
        Stats s{};
        for (const auto& e : entries) {
            if (!e.enabled) continue;
            const char* mod = decoderModuleName(e.decoder);
            if (mod[0] == '\0') continue;
            if (active_.find(e.id) != active_.end()) continue;
            const std::string vfoName  = vfoNameFor(e);
            const std::string instName = instanceNameFor(e);
            if (vfoExistsCb && !vfoExistsCb(vfoName)) {
                ++s.deferred;
                continue;
            }
            bool ok = false;
            if (createCb) ok = createCb(instName, mod, vfoName);
            if (!ok) {
                // Adoption: if the instance really does exist, treat
                // create-failure as success and re-track it.  This
                // closes the loop on a false-negative instanceExistsCb
                // in preTick that erased a still-live entry.
                if (instanceExistsCb && instanceExistsCb(instName)) {
                    Active a;
                    a.instance_name = instName;
                    a.decoder       = e.decoder;
                    a.bound_vfo     = vfoName;
                    active_[e.id]   = a;
                    ++s.spawned;
                    continue;
                }
                ++s.deferred;  // retry next frame
                continue;
            }
            Active a;
            a.instance_name = instName;
            a.decoder       = e.decoder;
            a.bound_vfo     = vfoName;
            active_[e.id]   = a;
            ++s.spawned;
        }
        return s;
    }

    // Drop everything (called on shutdown so destroyCb fires for every
    // live instance — required because ModuleManager won't otherwise
    // know to delete instances we spawned).
    void clear(const DestroyInstanceFn& destroyCb) {
        for (const auto& kv : active_) {
            if (destroyCb) destroyCb(kv.second.instance_name);
        }
        active_.clear();
    }

    // Diagnostics.
    std::size_t activeCount() const { return active_.size(); }
    bool isActive(const std::string& entryId) const {
        return active_.find(entryId) != active_.end();
    }
    std::string instanceNameOf(const std::string& entryId) const {
        auto it = active_.find(entryId);
        return (it == active_.end()) ? std::string() : it->second.instance_name;
    }

private:
    struct Active {
        std::string instance_name;
        DecoderKind decoder = DecoderKind::Radio_NBFM;
        std::string bound_vfo;
    };
    std::unordered_map<std::string, Active> active_;  // entry id -> Active
};

}  // namespace hold
}  // namespace predator
