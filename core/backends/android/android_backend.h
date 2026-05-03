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
    // Published by MainActivity.kt's setOnApplyWindowInsetsListener.
    // All values in raw screen pixels. Read each frame; safe to call from
    // the render thread (Kotlin side stores them as @Volatile Int).
    struct SafeAreaInsets {
        int top    = 0;
        int bottom = 0;
        int left   = 0;
        int right  = 0;
    };
    SafeAreaInsets getSafeAreaInsets();

    // Height in pixels currently occupied by the soft keyboard (0 when
    // hidden). The render loop subtracts this from DisplaySize.y so the
    // focused InputText doesn't end up under the IME.
    int getImeBottomInset();

    // Android PowerManager thermal status. 0 = NONE through 6 = SHUTDOWN.
    // Modules that drive heavy CPU (FFT, decoders) should back off when
    // this returns >= 3 (SEVERE).
    int getThermalStatus();
}