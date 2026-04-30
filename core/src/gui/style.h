#pragma once
#include <imgui.h>
#include <string>
#include <module.h>

namespace style {
    SDRPP_EXPORT ImFont* baseFont;
    SDRPP_EXPORT ImFont* bigFont;
    SDRPP_EXPORT ImFont* hugeFont;
    SDRPP_EXPORT float uiScale;

    // Scale the font atlas was rasterized at; used by the Display
    // menu to decide when the restart hint is needed.
    SDRPP_EXPORT float loadedFontScale;

    // Sentinel for the "Auto (device)" option. Stored in config as
    // the JSON string "auto"; in-memory it's -1.0f so it can't
    // collide with any supported positive scale.
    constexpr float AUTO_SCALE = -1.0f;

    bool setDefaultStyle(std::string resDir);
    bool loadFonts(std::string resDir);
    void beginDisabled();
    void endDisabled();
    void testtt();

    // Touch-friendly tweaks. Call after ScaleAllSizes(uiScale).
    void applyTouchFriendlyTweaks();

    // Clamp to [1.0, 4.0] then snap to one of the 11 supported steps.
    float snapToSupportedScale(float raw);

    // Resolve the "Auto (device)" option to a concrete snapped scale.
    float computeAutoScale();
}

namespace ImGui {
    void LeftLabel(const char* text);
    void FillWidth();
}