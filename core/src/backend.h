#pragma once
#include <string>
#include <vector>

#ifdef PREDATOR_BACKEND_WEB
// json.hpp is only required when building against the web backend.
// The include path is set by core/CMakeLists.txt when OPT_BACKEND_WEB=ON.
#include "json.hpp"
#endif

namespace backend {
    int init(std::string resDir = "");
    void beginFrame();
    void render(bool vsync = true);
    void getMouseScreenPos(double& x, double& y);
    void setMouseScreenPos(double x, double y);
    bool getPhoneLocation(double& lat, double& lon, float& accuracy, bool& hasFix);
    bool openMapView();

    float getNativeUiScale();
    bool isTouchPrimary();
    int renderLoop();
    int end();

    // Pixels currently occupied by the soft keyboard at the BOTTOM of the
    // screen, in raw screen pixels. Returns 0 when no keyboard is visible
    // and on backends that have no soft keyboard (desktop / GLFW). Used by
    // the GUI layer to keep modal text-edit popups from being covered by
    // the IME on Android, where Theme.NoTitleBar.Fullscreen + IMMERSIVE
    // sticky together defeat windowSoftInputMode="adjustResize" — the GL
    // surface stays full-screen so DisplaySize never shrinks for us.
    int getImeBottomInset();

    // Ask the platform IME for a numeric (digits + decimal point) soft
    // keyboard for subsequent text input instead of the full QWERTY layout.
    // Sticky until called again with false. No-op on backends without a
    // soft keyboard (desktop / GLFW / web). Used by the Fox Hunt tab's
    // frequency / duty-cycle / dead-man fields.
    void setImeNumeric(bool numeric);

    // Device safe-area insets (notch / status bar / nav bar) in raw screen
    // pixels. All zero on backends without insets (desktop / GLFW). The
    // GUI layer uses these to keep absolute-positioned popups clear of
    // camera cutouts and the system bars.
    struct SafeAreaInsets {
        int top    = 0;
        int bottom = 0;
        int left   = 0;
        int right  = 0;
    };
    SafeAreaInsets getSafeAreaInsets();

#ifdef PREDATOR_BACKEND_WEB
    // -------------------------------------------------------------------------
    // Web-backend live-data hooks (OPT_BACKEND_WEB only).
    // Only declared when PREDATOR_BACKEND_WEB is defined (set automatically by
    // core/CMakeLists.txt when OPT_BACKEND_WEB=ON). Non-web backends do not
    // need stubs — call sites in main_window.cpp are guarded by the same macro.
    //
    // See docs/linux_web_frontend.md for the full wire-up guide.
    // -------------------------------------------------------------------------

    // Push a fresh FFT snapshot.  Call from the releaseFFTBuffer path at the
    // same point the GLFW backend reads kujhadSpectrumRaw.
    void webBackendPushSpectrumSnapshot(const float* bins, int count,
                                         double centerHz, double bwHz,
                                         float fftMin, float fftMax);

    // Push a structured event (hit, anomaly, CoT export, etc.) into the SSE
    // ring so connected browser tabs receive it in real time.
    void webBackendPushEvent(const nlohmann::json& ev);

    // Replace the full nodes / tracks snapshot.  Call once per render tick
    // from the Kujhad snapshot refresh block in main_window.cpp.
    void webBackendUpdateNodes(const nlohmann::json& nodes);
    void webBackendUpdateTracks(const nlohmann::json& tracks);

    // Push a compact status snapshot (SDR state, mission mode, scan state).
    // Call once per render tick alongside the node/track update.
    void webBackendUpdateStatus(bool sdrRunning, double centerHz, double bwHz,
                                 const std::string& sourceName, int missionMode,
                                 bool scanRunning, const std::string& scanStatus);
#endif // PREDATOR_BACKEND_WEB
}
