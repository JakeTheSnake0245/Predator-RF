#include <gui/style.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <config.h>
#include <utils/flog.h>
#include <filesystem>
#include <algorithm>

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
        // Touch ergonomics: ImGui defaults assume a mouse cursor. After
        // ScaleAllSizes(uiScale), spacings are large in pixels but the
        // *grab regions* for sliders/scrollbars/window edges are still
        // designed for pixel-precise pointers. These tweaks ensure thumbs
        // can hit them reliably on a phone.
        //
        // All values are absolute (already-scaled) pixel sizes computed
        // from uiScale so they stay correct on tablets vs. phones.
        //
        // Gate on uiScale >= 1.5 so the desktop GLFW build (uiScale=1.0)
        // is unaffected — its ImGui style stays exactly as upstream SDR++
        // intends it. Any scaled build (Android at 3.0, hypothetical 4K
        // desktop config at 2.0+) gets the thumb-friendly minimums.
        if (uiScale < 1.5f) return;

        ImGuiStyle& s = ImGui::GetStyle();

        // Vertical/horizontal scrollbar thickness. Default is 14 px;
        // ScaleAllSizes brings that to 14*uiScale. We want at least 24*uiScale
        // for thumbs (≈7 mm at 440 PPI on uiScale=3).
        s.ScrollbarSize = std::max(s.ScrollbarSize, 24.0f * uiScale);

        // Slider grab: the draggable knob inside a slider track. Bump it
        // so it is comfortably wider than a fingertip's contact patch.
        s.GrabMinSize = std::max(s.GrabMinSize, 22.0f * uiScale);

        // Window/child border thickness so panel edges are visible against
        // the dark Diablo-tactical background on a small high-DPI screen.
        s.WindowBorderSize = std::max(s.WindowBorderSize, 1.0f);
        s.ChildBorderSize  = std::max(s.ChildBorderSize,  1.0f);
        s.FrameBorderSize  = std::max(s.FrameBorderSize,  1.0f);

        // Slightly increased rounding gives buttons/inputs a chunkier
        // tactile feel that reads better at finger-distance.
        s.FrameRounding   = std::max(s.FrameRounding,   2.0f * uiScale);
        s.GrabRounding    = std::max(s.GrabRounding,    2.0f * uiScale);
        s.ScrollbarRounding = std::max(s.ScrollbarRounding, 4.0f * uiScale);

        // Looser item spacing makes adjacent buttons less likely to be
        // hit accidentally by the same tap.
        if (s.ItemSpacing.y < 6.0f * uiScale) s.ItemSpacing.y = 6.0f * uiScale;

        // Touch-friendly resize grip in window corner.
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
