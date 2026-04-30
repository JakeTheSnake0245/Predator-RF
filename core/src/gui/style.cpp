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

    // Scale that the font atlas was last rasterized for. The Display
    // menu compares against this to decide whether the restart hint
    // is needed after a live scale change.
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
        return snapToSupportedScale(backend::getNativeUiScale());
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

        // Remember the scale we just baked into the font atlas so the
        // Display menu can decide later whether the user's new pick
        // requires a font rebuild (= app restart) or not.
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
        // Skip on desktop at the default scale; touch devices always
        // get the larger hit targets, even at 1.0x.
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

        s.TouchExtraPadding = ImVec2(4.0f * uiScale, 4.0f * uiScale);
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
