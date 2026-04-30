#pragma once
#include <imgui.h>
#include <string>
#include <module.h>

namespace style {
    SDRPP_EXPORT ImFont* baseFont;
    SDRPP_EXPORT ImFont* bigFont;
    SDRPP_EXPORT ImFont* hugeFont;
    SDRPP_EXPORT float uiScale;
    SDRPP_EXPORT float loadedFontScale;

    constexpr float AUTO_SCALE = -1.0f;

    bool setDefaultStyle(std::string resDir);
    bool loadFonts(std::string resDir);
    void beginDisabled();
    void endDisabled();
    void testtt();
    void applyTouchFriendlyTweaks();
    float snapToSupportedScale(float raw);
    float snapDownToSupportedScale(float raw);
    float computeAutoScale();
}

namespace ImGui {
    void LeftLabel(const char* text);
    void FillWidth();
}