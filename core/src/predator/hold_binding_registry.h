#pragma once

// Predator RF — Hold-binding registry.
//
// Why this exists
// ---------------
// HoldDecoderBinder (predator/hold_decoder_binder.h) spawns a decoder
// `ModuleManager::Instance` per held entry and points it at the
// pre-existing "Predator H<id>" VFO that HoldManager owns.  Decoder
// modules (e.g. rtl433_decoder) live in their own .so plugins and have
// no compile-time visibility into HoldManager / HoldDecoderBinder, and
// each module's per-instance config file is plugin-private — main_window
// cannot reach into it.
//
// This shared registry lets the Binder hand a decoder module the name
// of the VFO it should bind to.  The flow on spawn is:
//
//     1) Binder calls predator::hold::setBoundVfoFor("predator_hold_h7",
//                                                    "Predator H7")
//     2) Binder calls core::moduleManager.createInstance(
//                "predator_hold_h7", "rtl433_decoder")
//     3) Inside the new module's ctor:
//            std::string boundVfo =
//                predator::hold::getBoundVfoFor("predator_hold_h7");
//        Empty string  → legacy mode (module owns its own VFO).
//        Non-empty     → bound mode (skip own createVFO, hook the
//                                    sample stream from the named VFO).
//
// On teardown the Binder calls clearBoundVfoFor() AFTER deleteInstance
// so the (now-destroyed) module's ctor would fall back to legacy mode
// if a future instance somehow re-used the same name.
//
// Threading: register/unregister happen from the UI thread during
// Binder tick.  getBoundVfoFor() may be called from any thread.
//
// Lives in sdrpp_core (see hold_binding_registry.cpp) so every plugin
// shares the same instance.

#include <string>

namespace predator {
namespace hold {

// Record that a decoder module instance with the given name should bind
// to an existing VFO instead of creating its own.  Overwrites any
// previous binding for the same instance name.
void setBoundVfoFor(const std::string& instanceName,
                    const std::string& vfoName);

// Returns the bound VFO name registered for an instance, or "" if no
// binding is registered (legacy mode).
std::string getBoundVfoFor(const std::string& instanceName);

// Forget the binding for an instance.  Idempotent.
void clearBoundVfoFor(const std::string& instanceName);

// Diagnostic.
std::size_t boundInstanceCount();

}  // namespace hold
}  // namespace predator
