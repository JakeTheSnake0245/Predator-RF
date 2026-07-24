#pragma once
#include <string>
#include <vector>
#include <cmath>

/*
 * Node equipment calibration table.
 *
 * The AOU grid fuses received-power DIFFERENCES between nodes, which only
 * works if every node reports power on a comparable scale. Real hardware
 * doesn't: an RTL-SDR v3, an RTL clone, and a HackRF all read the same
 * signal several dB apart, and antenna gain adds on top. Each node
 * therefore declares its SDR type and antenna gain, and every hit row is
 * stamped with a single correction term:
 *
 *     calDb = sdrOffsetDb(type) + antennaGainDb
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

struct AntennaPreset {
    const char* label;
    double gainDb;
};

inline const std::vector<AntennaPreset>& antennaPresets() {
    static const std::vector<AntennaPreset> t = {
        { "Stock whip / rubber duck (0 dB)", 0.0 },
        { "Dipole kit (2 dB)",               2.0 },
        { "GMRS 3 dB",                       3.0 },
        { "GMRS 5 dB",                       5.0 },
        { "Discone (2 dB)",                  2.0 },
        { "Yagi 7 dB",                       7.0 },
        { "Custom",                          0.0 },
    };
    return t;
}

// The single number stamped onto hit rows as row["calDb"].
inline double calDb(const std::string& sdrId, double antennaGainDb) {
    double a = std::isfinite(antennaGainDb) ? antennaGainDb : 0.0;
    return sdrOffsetDb(sdrId) + a;
}

} // namespace predator::equipment
