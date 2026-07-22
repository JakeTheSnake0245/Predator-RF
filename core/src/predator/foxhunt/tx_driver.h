#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Fox Hunt TX driver interface + registry (roadmap: Fox Hunt TX tab).
//
// This is the ONLY transmit surface in Predator RF. It is deliberately kept
// separate from the RX sigpath and is LOCAL-HARDWARE-ONLY: the Kujhad HTTP,
// RNS cmd.v1 and predator-rfctl control-socket tx.* hard-rejects are
// untouched — no remote peer can ever reach this code path.
//
// Design:
//   * predator::foxhunt::TxDriver — abstract driver. Implementations live in
//     the source modules that already link the vendor libs (soapy_source →
//     SoapySDR TX, plutosdr_source → libiio TX) and register themselves at
//     module _INIT_ time, guarded by OPT_BUILD_FOXHUNT. This means NO new
//     Android Gradle native target is needed — the drivers ship inside
//     modules that are already wired into build.gradle.
//   * TxDriverRegistry — process-wide list, UI-thread only. main_window's
//     Fox Hunt tab enumerates through it and never includes vendor headers.
//   * Pure stdlib header so it syntax-checks standalone with
//     `g++ -std=c++17 -fsyntax-only -Icore/src`.
//
// Threading contract: enumerate()/open()/start()/stop()/close() are called
// from the UI thread only. write() and setGain() are called from the replay
// engine worker thread; drivers must make write() blocking (backpressure
// paces the worker) and setGain() safe to call concurrently with write().
// ─────────────────────────────────────────────────────────────────────────────
#include <complex>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace predator::foxhunt {

    struct TxDeviceInfo {
        std::string driver;       // registry key of the owning driver ("soapy", "plutosdr")
        std::string name;         // operator-facing label
        std::string id;           // driver-internal handle (soapy args string / iio uri)
        double minGainDb  = 0.0;  // driver-reported TX gain range
        double maxGainDb  = 0.0;
        double minSampleRate = 0.0;
        double maxSampleRate = 0.0;
        double minBandwidthHz = 0.0;  // 0/0 = driver cannot report; UI falls back
        double maxBandwidthHz = 0.0;  //         to the sample-rate bound
        bool   hasPowerEstimate = false;
        // Rough single-point estimate of output power at max gain, dBm.
        // Only meaningful when hasPowerEstimate is true. Purely informational.
        double estMaxPowerDbm = 0.0;
    };

    class TxDriver {
    public:
        virtual ~TxDriver() = default;

        // Short stable key ("soapy", "plutosdr"). Also used in config.json.
        virtual std::string key() const = 0;
        // Operator-facing name ("SoapySDR TX", "PlutoSDR TX").
        virtual std::string displayName() const = 0;

        // List TX-capable devices. May be slow (USB probe) — the UI calls it
        // only on explicit "Refresh".
        virtual std::vector<TxDeviceInfo> enumerate() = 0;

        // Open a device previously returned by enumerate(). Returns false and
        // fills err on failure. Must be idempotent-safe: open() on an already
        // open driver first closes the previous device.
        virtual bool open(const TxDeviceInfo& dev, std::string& err) = 0;

        // Configure and activate the TX stream. gainDb is within the device's
        // reported range.
        virtual bool start(double freqHz, double sampleRate, double bandwidthHz,
                           double gainDb, std::string& err) = 0;

        // Live gain update while streaming (slider drag).
        virtual void setGain(double gainDb) = 0;

        // Live frequency retune while streaming (Fox Hunt sweep mode).
        // Called from the UI thread; must be safe to call concurrently
        // with write() (same contract as setGain). Default: unsupported.
        // Returns false when the driver cannot retune without a full
        // stop()/start() cycle — the sweep stepper stops stepping and
        // reports it instead of glitching the stream.
        virtual bool setFrequency(double freqHz) { (void)freqHz; return false; }

        // Blocking write of interleaved complex float samples (full scale
        // ±1.0). Returns samples accepted, or <0 on stream error.
        virtual int write(const std::complex<float>* samples, int count) = 0;

        // Deactivate the TX stream (RF off). Device stays open.
        virtual void stop() = 0;

        // Release the device.
        virtual void close() = 0;
    };

    // Process-wide driver registry. Modules register at _INIT_, unregister at
    // _END_. The mutex only guards the vector — driver method calls follow
    // the threading contract above.
    class TxDriverRegistry {
    public:
        // Defined in tx_driver.cpp (compiled into sdrpp_core) so there is
        // exactly ONE registry instance process-wide even though driver
        // modules are separate shared objects. An inline/header definition
        // would risk one instance per DSO on platforms without vague-linkage
        // unification.
        static TxDriverRegistry& instance();

        void registerDriver(TxDriver* drv) {
            std::lock_guard<std::mutex> lck(mtx);
            for (auto* d : drivers) {
                if (d == drv) { return; }
            }
            drivers.push_back(drv);
        }

        void unregisterDriver(TxDriver* drv) {
            std::lock_guard<std::mutex> lck(mtx);
            for (auto it = drivers.begin(); it != drivers.end(); ++it) {
                if (*it == drv) { drivers.erase(it); return; }
            }
        }

        std::vector<TxDriver*> list() {
            std::lock_guard<std::mutex> lck(mtx);
            return drivers;
        }

        TxDriver* byKey(const std::string& key) {
            std::lock_guard<std::mutex> lck(mtx);
            for (auto* d : drivers) {
                if (d->key() == key) { return d; }
            }
            return nullptr;
        }

    private:
        std::mutex mtx;
        std::vector<TxDriver*> drivers;
    };
}
