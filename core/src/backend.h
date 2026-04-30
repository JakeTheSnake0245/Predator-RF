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

    // Returns the platform's native UI scale factor:
    // - On Android, this is android.util.DisplayMetrics.density (1.0 on
    //   ~160-dpi mdpi devices, 2.0 on xhdpi, 3.0 on xxhdpi, ~4.0 on a
    //   Samsung S22 portrait, etc.). Returns 1.0 if the JNI call fails.
    // - On desktop (GLFW), there's no comparable per-display density
    //   that maps cleanly to a touch-friendly scale, so it always
    //   returns 1.0 — desktop builds keep their current 100 % default.
    // The returned value is unsnapped; callers (style::computeAutoScale)
    // are responsible for clamping/snapping to the supported step list.
    float getNativeUiScale();
    int renderLoop();
    int end();
}
