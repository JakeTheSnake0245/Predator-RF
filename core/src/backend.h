#pragma once
#include <string>

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
}
