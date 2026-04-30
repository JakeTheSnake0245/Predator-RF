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

    // Native UI scale factor. Android: DisplayMetrics.density
    // (1.0 fallback on JNI failure). Desktop GLFW: 1.0.
    // Unsnapped; callers snap via style::snapToSupportedScale.
    float getNativeUiScale();

    // True when the primary input is a touchscreen (Android).
    // Used to gate touch-friendly style tweaks so desktop builds
    // keep their slim default look at uiScale == 1.0.
    bool isTouchPrimary();
    int renderLoop();
    int end();
}
