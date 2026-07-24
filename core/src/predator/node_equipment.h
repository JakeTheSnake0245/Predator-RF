#pragma once
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

/*
 * Node equipment calibration table.
 *
 * The AOU grid fuses received-power DIFFERENCES between nodes, which only
 * works if every node reports power on a comparable scale. Real hardware
 * doesn't: an RTL-SDR v3, an RTL clone, and a HackRF all read the same
 * signal several dB apart, and antenna gain adds on top. Each node
 * therefore declares its SDR type and antenna gain curve, and every hit
 * row is stamped with a correction term evaluated AT THE HIT'S FREQUENCY:
 *
 *     calDb(hit) = sdrOffsetDb(type) + antennaGainAt(curve, hit.freq)
 *     comparable_power = strengthDb - calDb
 *
 * The offsets below are engineering approximations of typical front-end
 * sensitivity relative to the RTL-SDR Blog v3 (the fleet reference, 0 dB).
 * They don't need to be perfect — the PDOA math already carries a 6 dB
 * noise sigma — they need to remove the BULK of the hardware skew so a
 * hot-reading node doesn't systematically drag position estimates toward
 * itself. Field-calibrate with a beacon at a known spot for the last dB.
 */

namespace predator::equipment {

struct SdrProfile {
    const char* id;
    const char* label;
    double offsetDb;   // typical reading offset vs RTL-SDR Blog v3
};

inline const std::vector<SdrProfile>& sdrProfiles() {
    static const std::vector<SdrProfile> t = {
        { "rtlsdr_v3",    "RTL-SDR Blog v3 (reference)",  0.0 },
        { "rtlsdr_v4",    "RTL-SDR Blog v4",              0.5 },
        { "rtlsdr_clone", "Generic RTL2832 clone",       -2.0 },
        { "nesdr",        "Nooelec NESDR",               -0.5 },
        { "hackrf",       "HackRF One",                  -4.0 },
        { "hackrf_clone", "HackRF clone",                -6.5 },
        { "airspy_mini",  "Airspy Mini",                  1.5 },
        { "unknown",      "Other / unknown",              0.0 },
    };
    return t;
}

inline double sdrOffsetDb(const std::string& id) {
    for (auto& p : sdrProfiles()) {
        if (id == p.id) return p.offsetDb;
    }
    return 0.0;
}

inline int sdrProfileIndex(const std::string& id) {
    const auto& t = sdrProfiles();
    for (size_t i = 0; i < t.size(); i++) {
        if (id == t[i].id) return (int)i;
    }
    return (int)t.size() - 1;   // "unknown"
}

/*
 * Antenna gain is FREQUENCY-DEPENDENT: a GMRS whip that's +5 dB at
 * 465 MHz is typically well below isotropic at 900 MHz ISM, and a
 * 900 MHz yagi is useless at VHF. Since the fleet hunts arbitrary
 * frequencies (not one standard band), each node declares its antenna
 * as a gain CURVE — a few (freqMhz, gainDb) points — and every hit is
 * corrected at the hit's own frequency.
 *
 * Interpolation is linear in log10(frequency) (antenna responses are
 * smoother in log-f); outside the declared points the nearest end
 * value is held. A single-point curve therefore acts as a flat gain.
 */

struct GainPoint {
    double freqMhz;
    double gainDb;
};
using AntennaCurve = std::vector<GainPoint>;

inline double antennaGainAt(const AntennaCurve& curve, double freqHz) {
    if (curve.empty()) return 0.0;
    AntennaCurve s = curve;
    std::sort(s.begin(), s.end(), [](const GainPoint& a, const GainPoint& b) {
        return a.freqMhz < b.freqMhz;
    });
    double f = freqHz / 1e6;
    // Invalid/zero frequency: no basis to pick a band, so apply NO antenna
    // correction (0 dB) rather than arbitrarily holding the first point.
    if (!std::isfinite(f) || f <= 0.0) return 0.0;
    if (f <= s.front().freqMhz) return s.front().gainDb;
    if (f >= s.back().freqMhz) return s.back().gainDb;
    for (size_t i = 1; i < s.size(); i++) {
        if (f <= s[i].freqMhz) {
            double f0 = std::log10(std::max(s[i - 1].freqMhz, 0.001));
            double f1 = std::log10(std::max(s[i].freqMhz, 0.001));
            double t = (f1 > f0) ? (std::log10(f) - f0) / (f1 - f0) : 0.0;
            t = std::clamp(t, 0.0, 1.0);
            return s[i - 1].gainDb + t * (s[i].gainDb - s[i - 1].gainDb);
        }
    }
    return s.back().gainDb;
}

// Presets are starting POINTS, not whole curves: picking one adds a
// (freq, gain) point at the band the antenna is actually built for. The
// operator adds more points to describe off-band behavior.
struct AntennaPreset {
    const char* label;
    double freqMhz;
    double gainDb;
};

inline const std::vector<AntennaPreset>& antennaPresets() {
    static const std::vector<AntennaPreset> t = {
        { "Stock whip / rubber duck (0 dB, wideband)", 400.0, 0.0 },
        { "VHF dipole (2 dB @ 150 MHz)",               150.0, 2.0 },
        { "GMRS 3 dB @ 465 MHz",                       465.0, 3.0 },
        { "GMRS 5 dB @ 465 MHz",                       465.0, 5.0 },
        { "900 MHz ISM 5 dB @ 915 MHz",                915.0, 5.0 },
        { "900 MHz ISM 8 dB @ 915 MHz",                915.0, 8.0 },
        { "Discone (2 dB, wideband @ 400 MHz)",        400.0, 2.0 },
        { "Yagi 7 dB @ 465 MHz",                       465.0, 7.0 },
    };
    return t;
}

// The number stamped onto hit rows as row["calDb"], evaluated at the
// hit's frequency.
inline double calDbAt(const std::string& sdrId, const AntennaCurve& curve, double freqHz) {
    double a = antennaGainAt(curve, freqHz);
    if (!std::isfinite(a)) a = 0.0;
    return sdrOffsetDb(sdrId) + a;
}

} // namespace predator::equipment
