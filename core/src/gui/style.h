#pragma once
#include <imgui.h>
#include <string>
#include <module.h>

namespace style {
    SDRPP_EXPORT ImFont* baseFont;
    SDRPP_EXPORT ImFont* bigFont;
    SDRPP_EXPORT ImFont* hugeFont;
    SDRPP_EXPORT float uiScale;

    // The uiScale value that was in effect when loadFonts() last ran,
    // i.e. what the rasterized font atlas was sized for. Live scale
    // changes update everything except the font atlas; the Display menu
    // compares the new uiScale against this to decide whether the
    // "Restart required." hint should appear.
    SDRPP_EXPORT float loadedFontScale;

    // Sentinel value that means "use backend::getNativeUiScale()" on
    // every read. Stored in the in-memory uiScale combo's option list
    // (key + value) and in config as the JSON string "auto" — kept
    // separate from any real positive scale so it can never collide
    // with a legitimate manual choice.
    constexpr float AUTO_SCALE = -1.0f;

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

    // Snaps an arbitrary positive scale factor to the nearest entry in
    // the supported step list (1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 2.5,
    // 2.75, 3.0, 3.5, 4.0). Values outside [1.0, 4.0] are clamped first
    // so the Auto path never produces a bigger style than the user can
    // reach manually. Never returns AUTO_SCALE.
    float snapToSupportedScale(float raw);

    // Asks the backend for the native display density (Android:
    // DisplayMetrics.density; desktop: 1.0) and snaps the result. This
    // is what the "Auto (device)" option resolves to at runtime.
    float computeAutoScale();
}

namespace ImGui {
    void LeftLabel(const char* text);
    void FillWidth();
}