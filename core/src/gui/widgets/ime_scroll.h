#pragma once
#include <imgui.h>
#include <backend.h>

// Android soft-keyboard visibility helper.
//
// With windowSoftInputMode="adjustNothing" the IME floats OVER the GL
// surface, so an inline InputText low on the screen ends up hidden behind
// the keyboard while the user types.  Call imeScrollIntoView() immediately
// after ANY ImGui::Input* widget that lives in a scrollable region (menu
// column, popup child, etc).  It scrolls the just-activated field into the
// upper portion of the scroll region (above the IME line), and re-scrolls
// if the IME rises while the field is already active (the inset is 0 at
// activation time and only becomes non-zero once the IME animates up).
//
// On desktop backends backend::getImeBottomInset() returns 0, so this
// degrades to a centre-on-activate convenience and is otherwise harmless.
//
// The rose-edge detection is frame-global (keyed on ImGui::GetFrameCount())
// so the helper works correctly with any number of call sites per frame.
namespace ImGui {
    inline void imeScrollIntoView() {
        static int prevBot = 0;
        static int lastFrame = -1;
        static bool rose = false;
        int frame = ImGui::GetFrameCount();
        int cur = backend::getImeBottomInset();
        if (frame != lastFrame) {
            rose = (cur > prevBot);
            prevBot = cur;
            lastFrame = frame;
        }
        // IME up: bias the field toward the top quarter so it sits above
        // the keyboard. IME down: centre it.
        float frac = (cur > 0) ? 0.25f : 0.5f;
        if (ImGui::IsItemActivated() || (rose && ImGui::IsItemActive())) {
            ImGui::SetScrollHereY(frac);
        }
    }

    // Drop-in wrappers for `if (ImGui::InputX(...)) { ... }` call sites:
    // draw the widget, keep it visible above the IME, return its result.
    inline bool InputTextIME(const char* label, char* buf, size_t buf_size, ImGuiInputTextFlags flags = 0) {
        bool ret = ImGui::InputText(label, buf, buf_size, flags);
        imeScrollIntoView();
        return ret;
    }
    inline bool InputIntIME(const char* label, int* v, int step = 1, int step_fast = 100, ImGuiInputTextFlags flags = 0) {
        bool ret = ImGui::InputInt(label, v, step, step_fast, flags);
        imeScrollIntoView();
        return ret;
    }
    inline bool InputFloatIME(const char* label, float* v, float step = 0.0f, float step_fast = 0.0f, const char* format = "%.3f", ImGuiInputTextFlags flags = 0) {
        bool ret = ImGui::InputFloat(label, v, step, step_fast, format, flags);
        imeScrollIntoView();
        return ret;
    }
    inline bool InputDoubleIME(const char* label, double* v, double step = 0.0, double step_fast = 0.0, const char* format = "%.6f", ImGuiInputTextFlags flags = 0) {
        bool ret = ImGui::InputDouble(label, v, step, step_fast, format, flags);
        imeScrollIntoView();
        return ret;
    }
}
