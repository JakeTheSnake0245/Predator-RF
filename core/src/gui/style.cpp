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

    float snapDownToSupportedScale(float raw) {
        if (!(raw > 0.0f)) raw = SUPPORTED_SCALES.front();
        if (raw <= SUPPORTED_SCALES.front()) return SUPPORTED_SCALES.front();
        if (raw >= SUPPORTED_SCALES.back())  return SUPPORTED_SCALES.back();
        float best = SUPPORTED_SCALES.front();
        for (float s : SUPPORTED_SCALES) {
            if (s <= raw + 1e-4f) best = s;
            else break;
        }
        return best;
    }

    float computeAutoScale() {
        float density = backend::getNativeUiScale();
        if (!backend::isTouchPrimary()) {
            return snapDownToSupportedScale(density);
        }

        const float MAIN_CHROME_PER_UNIT = 8.0f + 42.0f + 8.0f + 46.0f + 8.0f + 8.0f;

        // Right rail composition after the Zoom/Max/Min sliders moved
        // into the top-right waterfall dropdown overlay (Task #20):
        // only the 7 tab buttons + their inter-spacing + child border
        // padding remain. The slider/separator block is no longer in
        // the rail so we drop it from the per-unit pixel budget,
        // otherwise computeAutoScale would over-shrink on touch.
        const float TAB_HEIGHT       = 36.0f;
        const float TAB_COUNT        = 7.0f;
        const float ITEM_SPACING_Y   = 6.0f;
        const float CHILD_PADDING    = 2.0f * 8.0f + 2.0f;

        const float RAIL_PER_UNIT =
            TAB_COUNT * TAB_HEIGHT
            + (TAB_COUNT - 1.0f) * ITEM_SPACING_Y
            + CHILD_PADDING;

        const float TOTAL_PER_UNIT = MAIN_CHROME_PER_UNIT + RAIL_PER_UNIT;

        float raw = density;
        int h = backend::getDisplayHeightPx();
        if (h > 0 && TOTAL_PER_UNIT > 0.0f) {
            float fit = (float)h / TOTAL_PER_UNIT;
            if (fit < raw) raw = fit;
        }

        if (raw < 1.5f) raw = 1.5f;
        return snapDownToSupportedScale(raw);
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
