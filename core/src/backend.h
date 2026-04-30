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
    int getDisplayHeightPx();
    int getDisplayWidthPx();
    // Bottom-edge inset in pixels covered by the on-screen keyboard
    // (Android IME). 0 when no keyboard is showing or on desktop.
    int getImeBottomInsetPx();
    int renderLoop();
    int end();
}
