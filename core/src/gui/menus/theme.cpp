#include <gui/menus/theme.h>
#include <gui/gui.h>
#include <core.h>
#include <gui/style.h>

namespace thememenu {
    int themeId;
    std::vector<std::string> themeNames;
    std::string themeNamesTxt;

    void init(std::string resDir) {
        // TODO: Not hardcode theme directory
        gui::themeManager.loadThemesFromDir(resDir + "/themes/");
        core::configManager.acquire();
        std::string selectedThemeName = core::configManager.conf["theme"];
        core::configManager.release();

        // Select theme by name, if not available, apply Predator RF then Dark
        themeNames = gui::themeManager.getThemeNames();
        auto it = std::find(themeNames.begin(), themeNames.end(), selectedThemeName);
        if (it == themeNames.end()) {
            it = std::find(themeNames.begin(), themeNames.end(), "Predator RF");
            selectedThemeName = "Predator RF";
        }
        if (it == themeNames.end()) {
            it = std::find(themeNames.begin(), themeNames.end(), "SDR Predator");
            selectedThemeName = "SDR Predator";
        }
        if (it == themeNames.end()) {
            it = std::find(themeNames.begin(), themeNames.end(), "Dark");
            selectedThemeName = "Dark";
        }
        themeId = std::distance(themeNames.begin(), it);
        // applyTheme() now also handles ScaleAllSizes(uiScale) and the
        // touch-friendly ergonomics tweaks, so a single call here gives
        // us the fully-styled, fully-scaled, touch-ready ImGuiStyle in
        // one shot. Doing it inside applyTheme() also means runtime
        // theme switches via the dropdown can never silently lose the
        // scaling or the touch tweaks (ThemeManager::applyTheme calls
        // ImGui::StyleColorsDark() internally, which resets the style).
        applyTheme();

        themeNamesTxt = "";
        for (auto name : themeNames) {
            themeNamesTxt += name;
            themeNamesTxt += '\0';
        }
    }

     void applyTheme() {
         // ThemeManager::applyTheme calls ImGui::StyleColorsDark() and then
         // overwrites rounding/border/padding/spacing with unscaled pixel
         // constants — appropriate for a desktop mouse but tiny on a
         // 3x-scaled phone screen. Re-running ScaleAllSizes(uiScale) here
         // brings everything back to the right physical size, and
         // applyTouchFriendlyTweaks() then enforces minimum thumb-friendly
         // sizes for scrollbars / slider grabs / borders so that switching
         // themes at runtime never regresses Android touch ergonomics.
         gui::themeManager.applyTheme(themeNames[themeId]);
         ImGui::GetStyle().ScaleAllSizes(style::uiScale);
         style::applyTouchFriendlyTweaks();
     }

    void draw(void* ctx) {
        float menuWidth = ImGui::GetContentRegionAvail().x;
        ImGui::LeftLabel("Theme");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo("##theme_select_combo", &themeId, themeNamesTxt.c_str())) {
            applyTheme();
            core::configManager.acquire();
            core::configManager.conf["theme"] = themeNames[themeId];
            core::configManager.release(true);
        }
    }
}
