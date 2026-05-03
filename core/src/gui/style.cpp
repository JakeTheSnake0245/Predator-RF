#include <gui/style.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <config.h>
#include <utils/flog.h>
#include <filesystem>
#include <algorithm>
#include <array>
#include <cmath>
#include <backend.h>

namespace style {
    ImFont* baseFont;
    ImFont* bigFont;
    ImFont* hugeFont;
    ImVector<ImWchar> baseRanges;
    ImVector<ImWchar> bigRanges;
    ImVector<ImWchar> hugeRanges;

#ifndef __ANDROID__
    float uiScale = 1.0f;
#else
    float uiScale = 3.0f;
#endif

#ifndef __ANDROID__
    float loadedFontScale = 1.0f;
#else
    float loadedFontScale = 3.0f;
#endif

    static const std::array<float, 11> SUPPORTED_SCALES = {
        1.00f, 1.25f, 1.50f, 1.75f, 2.00f,
        2.25f, 2.50f, 2.75f, 3.00f, 3.50f, 4.00f
    };

    float snapToSupportedScale(float raw) {
        if (!(raw > 0.0f)) raw = 1.0f;
        float clamped = std::clamp(raw, SUPPORTED_SCALES.front(), SUPPORTED_SCALES.back());
        float best = SUPPORTED_SCALES.front();
        float bestDist = std::fabs(clamped - best);
        for (float s : SUPPORTED_SCALES) {
            float d = std::fabs(clamped - s);
            if (d < bestDist) { best = s; bestDist = d; }
        }
        return best;
    }

    float computeAutoScale() {
        float raw = backend::getNativeUiScale();
        if (backend::isTouchPrimary() && raw < 1.5f) raw = 1.5f;
        return snapToSupportedScale(raw);
    }

    bool loadFonts(std::string resDir) {
        ImFontAtlas* fonts = ImGui::GetIO().Fonts;
        if (!std::filesystem::is_directory(resDir)) {
            flog::error("Invalid resource directory: {0}", resDir);
            return false;
        }

        // Create base font range
        ImFontGlyphRangesBuilder baseBuilder;
        baseBuilder.AddRanges(fonts->GetGlyphRangesDefault());
        baseBuilder.AddRanges(fonts->GetGlyphRangesCyrillic());
        baseBuilder.BuildRanges(&baseRanges);

        // Create big font range
        ImFontGlyphRangesBuilder bigBuilder;
        const ImWchar bigRange[] = { '.', '9', 0 };
        bigBuilder.AddRanges(bigRange);
        bigBuilder.BuildRanges(&bigRanges);

        // Create huge font range
        ImFontGlyphRangesBuilder hugeBuilder;
        const ImWchar hugeRange[] = { 'S', 'S', 'D', 'D', 'R', 'R', '+', '+', ' ', ' ', 0 };
        hugeBuilder.AddRanges(hugeRange);
        hugeBuilder.BuildRanges(&hugeRanges);
        
        // Add bigger fonts for frequency select and title
        baseFont = fonts->AddFontFromFileTTF(((std::string)(resDir + "/fonts/Roboto-Medium.ttf")).c_str(), 16.0f * uiScale, NULL, baseRanges.Data);
        bigFont = fonts->AddFontFromFileTTF(((std::string)(resDir + "/fonts/Roboto-Medium.ttf")).c_str(), 45.0f * uiScale, NULL, bigRanges.Data);
        hugeFont = fonts->AddFontFromFileTTF(((std::string)(resDir + "/fonts/Roboto-Medium.ttf")).c_str(), 128.0f * uiScale, NULL, hugeRanges.Data);

        loadedFontScale = uiScale;

        return true;
    }

    void beginDisabled() {
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        auto& style = ImGui::GetStyle();
        ImVec4* colors = style.Colors;
        ImVec4 btnCol = colors[ImGuiCol_Button];
        ImVec4 frameCol = colors[ImGuiCol_FrameBg];
        ImVec4 textCol = colors[ImGuiCol_Text];
        btnCol.w = 0.15f;
        frameCol.w = 0.30f;
        textCol.w = 0.65f;
        ImGui::PushStyleColor(ImGuiCol_Button, btnCol);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, frameCol);
        ImGui::PushStyleColor(ImGuiCol_Text, textCol);
    }

    void endDisabled() {
        ImGui::PopItemFlag();
        ImGui::PopStyleColor(3);
    }

    void applyTouchFriendlyTweaks() {
        if (!backend::isTouchPrimary() && uiScale <= 1.0001f) return;

        ImGuiStyle& s = ImGui::GetStyle();

        s.ScrollbarSize = std::max(s.ScrollbarSize, 32.0f * uiScale);
        s.GrabMinSize   = std::max(s.GrabMinSize,   32.0f * uiScale);

        s.WindowBorderSize = std::max(s.WindowBorderSize, 1.0f);
        s.ChildBorderSize  = std::max(s.ChildBorderSize,  1.0f);
        s.FrameBorderSize  = std::max(s.FrameBorderSize,  1.0f);

        s.FrameRounding     = std::max(s.FrameRounding,     2.0f * uiScale);
        s.GrabRounding      = std::max(s.GrabRounding,      2.0f * uiScale);
        s.ScrollbarRounding = std::max(s.ScrollbarRounding, 4.0f * uiScale);

        if (s.ItemSpacing.y  < 6.0f  * uiScale) s.ItemSpacing.y  = 6.0f  * uiScale;
        if (s.FramePadding.y < 6.0f  * uiScale) s.FramePadding.y = 6.0f  * uiScale;
        if (s.IndentSpacing  < 18.0f * uiScale) s.IndentSpacing  = 18.0f * uiScale;

        // TouchExtraPadding extends every widget's hit-test area by this
        // many pixels in each direction. Bumped substantially on touch
        // devices so that a thumb tap whose centroid lands a few mm
        // outside the widget — or whose finger drifts during press-and-
        // lift — still counts as a hit. Without this, normal taps on
        // small widgets (combo arrows, slider thumbs, sub-menu rows)
        // miss because the visible widget rect is smaller than the
        // finger contact patch.
        s.TouchExtraPadding = ImVec2(16.0f * uiScale, 16.0f * uiScale);

        // ── CRITICAL: drag/click thresholds for finger input ──────────────
        // ImGui defaults (6 px MouseDragThreshold, 6 px MouseDoubleClickMaxDist)
        // are tuned for a mouse with sub-pixel precision. On a 3x-scaled phone
        // screen, normal finger jitter between ACTION_DOWN and ACTION_UP is
        // routinely 10–25 raw pixels — which trips ImGui's drag detector
        // BEFORE the click is registered. Net effect: every tap on a button,
        // tab, menu item, or list row is silently re-classified as a drag,
        // and the widget never fires its Clicked() event. The waterfall and
        // any scroll container then consume the motion as a swipe.
        //
        // Bumping the drag threshold to 20*uiScale (≈40–60 raw px on phones)
        // means the finger has to actually move a visible distance before
        // ImGui treats the gesture as a drag. Taps with normal jitter now
        // resolve to clicks. We also raise the double-click max distance so
        // double-taps survive the same jitter.
        ImGuiIO& io = ImGui::GetIO();
        io.MouseDragThreshold     = std::max(io.MouseDragThreshold,     20.0f * uiScale);
        io.MouseDoubleClickMaxDist = std::max(io.MouseDoubleClickMaxDist, 16.0f * uiScale);
    }
}

namespace ImGui {
    void LeftLabel(const char* text) {
        float vpos = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(vpos + GImGui->Style.FramePadding.y);
        ImGui::TextUnformatted(text);
        ImGui::SameLine();
        ImGui::SetCursorPosY(vpos);
    }

    void FillWidth() {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    }
}
