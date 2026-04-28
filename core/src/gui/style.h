#pragma once
#include <imgui.h>
#include <string>
#include <module.h>

namespace style {
    SDRPP_EXPORT ImFont* baseFont;
    SDRPP_EXPORT ImFont* bigFont;
    SDRPP_EXPORT ImFont* hugeFont;
    SDRPP_EXPORT float uiScale;

    bool setDefaultStyle(std::string resDir);
    bool loadFonts(std::string resDir);
    void beginDisabled();
    void endDisabled();
    void testtt();

    // Touch-friendly tweaks for phone/tablet builds.
    // Bumps scrollbar width, slider grab size, frame border, and rounding
    // so taps with a finger land reliably. Call once after
    // ImGui::GetStyle().ScaleAllSizes(uiScale).
    void applyTouchFriendlyTweaks();
}

namespace ImGui {
    void LeftLabel(const char* text);
    void FillWidth();
}