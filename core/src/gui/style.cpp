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

    // Tracks what the font atlas was actually rasterized for. loadFonts()
    // overwrites this; the Display menu uses it to decide whether the
    // "Restart required." hint is needed after a live scale change.
    // Initialized to the same default uiScale would have so the first
    // load always agrees with itself.
#ifndef __ANDROID__
    float loadedFontScale = 1.0f;
#else
    float loadedFontScale = 3.0f;
#endif

    // Canonical list of UI scale steps offered by the Display menu and
    // used as snap targets for the Auto path. Coarse below 1.5x because
    // anything below that is desktop-style mouse use (where exact
    // pixel-precision matters), denser in the 1.5x–3.0x band where
    // most phones live, then a sparse 3.5/4.0 for tablets.
    static const std::array<float, 11> SUPPORTED_SCALES = {
        1.00f, 1.25f, 1.50f, 1.75f, 2.00f,
        2.25f, 2.50f, 2.75f, 3.00f, 3.50f, 4.00f
    };

    float snapToSupportedScale(float raw) {
        // Clamp first so absurd values (a future huge-DPI device, or a
        // legacy config from a build that allowed >4x) collapse to the
        // largest supported step instead of throwing OptionList off.
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
        // backend::getNativeUiScale() already guards against bogus
        // densities and returns 1.0 on failure. We just snap.
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
        // Touch ergonomics: ImGui defaults assume a mouse cursor. After
        // ScaleAllSizes(uiScale), spacings are large in pixels but the
        // *grab regions* for sliders/scrollbars/window edges are still
        // designed for pixel-precise pointers. These tweaks ensure thumbs
        // can hit them reliably on a phone.
        //
        // All values are absolute (already-scaled) pixel sizes computed
        // from uiScale so they stay correct on tablets vs. phones.
        //
        // Skip ONLY when uiScale is exactly 1.0 (i.e. desktop GLFW
        // default). The previous gate of `>= 1.5` left small-density
        // phones (uiScale 1.25) with the upstream desktop look, which
        // is the bug that motivated finer scale steps in the first
        // place — anything above the desktop default is presumed to be
        // a touch device that wants the chunky minimums.
        if (uiScale <= 1.0001f) return;

        ImGuiStyle& s = ImGui::GetStyle();

        // Vertical/horizontal scrollbar thickness. Default is 14 px;
        // ScaleAllSizes brings that to 14*uiScale. Bumped to 32*uiScale
        // (~9.5 mm at 440 PPI on uiScale=3) after on-device testing on a
        // Samsung S22 portrait — 24*uiScale was still hard to grab with a
        // thumb when the right panel scrollbar shared the screen edge.
        s.ScrollbarSize = std::max(s.ScrollbarSize, 32.0f * uiScale);

        // Slider grab: the draggable knob inside a slider track AND the
        // thumb inside a scrollbar. ImGui caps the thumb size to the
        // content's "natural" size, so a long list produces a tiny thumb;
        // GrabMinSize is what keeps it thumb-grabbable. Bumped to 32*uiScale
        // so even a long event log keeps a chunky scroll thumb.
        s.GrabMinSize = std::max(s.GrabMinSize, 32.0f * uiScale);

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

        // FramePadding drives the *height* of every collapsing-header
        // (the side menu's section bars), checkbox row, combo box, and
        // input — i.e. every tap target in the side menu. ImGui's
        // default y-padding of 3 px scales to 3*uiScale, which still
        // isn't a comfortable thumb target at 1.25x or 1.5x. Floor it
        // at 6*uiScale (~9.5 mm at 440 PPI on uiScale=3) so the section
        // bars stay finger-sized at every supported scale.
        if (s.FramePadding.y < 6.0f * uiScale) s.FramePadding.y = 6.0f * uiScale;

        // IndentSpacing sets the left inset for sub-menu items inside a
        // collapsing header. ImGui defaults to 21 px; on a high-DPI
        // phone that's a thin sliver of padding that crowds sub-items
        // against the section's left edge. Bumping to a sensible
        // physical inset gives sub-menus visual hierarchy AND extra
        // horizontal room around their tap targets.
        if (s.IndentSpacing < 18.0f * uiScale) s.IndentSpacing = 18.0f * uiScale;

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
