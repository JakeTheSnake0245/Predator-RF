#pragma once
#include <vector>
#include <stdint.h>

namespace backend {
    struct DevVIDPID {
        uint16_t vid;
        uint16_t pid;
    };

    extern const std::vector<DevVIDPID> AIRSPY_VIDPIDS;
    extern const std::vector<DevVIDPID> AIRSPYHF_VIDPIDS;
    extern const std::vector<DevVIDPID> HACKRF_VIDPIDS;
    extern const std::vector<DevVIDPID> RTL_SDR_VIDPIDS;

    int getDeviceFD(int& vid, int& pid, const std::vector<DevVIDPID>& allowedVidPids);

    // ── Window insets / IME ─────────────────────────────────────────────
    // SafeAreaInsets struct + getSafeAreaInsets() + getImeBottomInset()
    // are declared in the cross-platform <backend.h> so the GUI layer can
    // call them from any backend. Both are published here by
    // MainActivity.kt's setOnApplyWindowInsetsListener (Kotlin side stores
    // the values as @Volatile Int so the render thread can read them
    // lock-free).

    // Android PowerManager thermal status. 0 = NONE through 6 = SHUTDOWN.
    // Modules that drive heavy CPU (FFT, decoders) should back off when
    // this returns >= 3 (SEVERE).
    int getThermalStatus();
}