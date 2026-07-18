#pragma once
#include <string>
#include <vector>
#include <memory>

// Fox Hunt TX driver abstraction (Task: Fox Hunt TX tab).
//
// This is the ONLY transmit surface in Predator RF. It is deliberately kept
// completely separate from the RX sigpath (SourceManager) so that:
//   1. RX-only builds can exclude it entirely (OPT_BUILD_FOXHUNT=OFF).
//   2. The remote tx.* rejection posture (Kujhad / RNS / control socket /
//      web /api/command) is untouched — nothing here is reachable from any
//      network command path. Local hardware, local operator, local UI only.
//
// Drivers implement this interface. SoapySDR is the generic driver on Linux;
// PlutoSDR (libiio) is the native driver available on both Linux and Android.
// HackRF / LimeSDR / USRP can be added later either natively or (on Linux)
// through their Soapy support modules without touching the replay engine.

namespace foxhunt {

    struct TxDeviceInfo {
        std::string driverId; // "soapy", "pluto", ...
        std::string label;    // Human readable, shown in the combo
        std::string uri;      // Driver-specific open string (soapy args / iio uri)
    };

    struct TxCaps {
        double gainMin = 0.0;
        double gainMax = 0.0;
        double gainStep = 1.0;
        // Sorted list of supported sample rates (Hz). Empty = continuous range,
        // use srMin/srMax.
        std::vector<double> sampleRates;
        double srMin = 0.0;
        double srMax = 0.0;
        // Analog filter bandwidth range (Hz). bwMax==0 -> bandwidth not settable.
        double bwMin = 0.0;
        double bwMax = 0.0;
        double freqMin = 0.0;
        double freqMax = 0.0;
        // True when the driver can estimate output power in dBm for a given
        // gain setting (most cannot — UI then shows plain gain dB).
        bool hasPowerEstimate = false;
        // dBm at gainMax with a full-scale signal (only if hasPowerEstimate).
        double maxPowerDbm = 0.0;
    };

    class TxDriver {
    public:
        virtual ~TxDriver() {}

        virtual std::string id() = 0;

        // Enumerate currently attached TX-capable devices for this driver.
        virtual std::vector<TxDeviceInfo> enumerate() = 0;

        // Query capabilities. May briefly open the device. Returns false and
        // sets err when the device cannot be probed.
        virtual bool caps(const TxDeviceInfo& dev, TxCaps& out, std::string& err) = 0;

        // Open the device for transmit. Frequency in Hz, gain in dB (driver
        // native units), bandwidth in Hz (0 = auto/skip).
        virtual bool open(const TxDeviceInfo& dev, double sampleRate,
                          double freqHz, double bwHz, double gainDb,
                          std::string& err) = 0;

        // Live-adjust while TX is running (or between bursts).
        virtual void setFrequency(double freqHz) = 0;
        virtual void setGain(double gainDb) = 0;

        // Blocking write of interleaved complex float32 IQ (count = IQ pairs,
        // full scale ±1.0). Returns pairs consumed, or <0 on error.
        virtual int write(const float* iq, int count) = 0;

        // Flush + stop the stream and close the device. Must be safe to call
        // twice and safe to call when open() failed.
        virtual void close() = 0;
    };

    // Registry filled at module init with whichever drivers were compiled in.
    inline std::vector<std::shared_ptr<TxDriver>>& driverRegistry() {
        static std::vector<std::shared_ptr<TxDriver>> reg;
        return reg;
    }
}
