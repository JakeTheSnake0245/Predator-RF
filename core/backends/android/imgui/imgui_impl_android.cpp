// dear imgui: Platform Binding for Android native app
// This needs to be used along with the OpenGL 3 Renderer (imgui_impl_opengl3)

// Implemented features:
//  [X] Platform: Keyboard support. Since 1.87 we are using the io.AddKeyEvent() function. Pass ImGuiKey values to all key functions e.g. ImGui::IsKeyPressed(ImGuiKey_Space). [Legacy AKEYCODE_* values will also be supported unless IMGUI_DISABLE_OBSOLETE_KEYIO is set]
// Missing features:
//  [ ] Platform: Clipboard support.
//  [ ] Platform: Gamepad support. Enable with 'io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad'.
//  [ ] Platform: Mouse cursor shape and visibility. Disable with 'io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange'. FIXME: Check if this is even possible with Android.
// Important:
//  - Consider using SDL or GLFW backend on Android, which will be more full-featured than this.
//  - FIXME: On-screen keyboard currently needs to be enabled by the application (see examples/ and issue #3446)
//  - FIXME: Unicode character inputs needs to be passed by Dear ImGui by the application (see examples/ and issue #3446)

// You can use unmodified imgui_impl_* files in your project. See examples/ folder for examples of using this.
// Prefer including the entire imgui/ repository into your project (either as a copy or as a submodule), and only build the backends you need.
// If you are new to Dear ImGui, read documentation from the docs/ folder + read the top of imgui.cpp.
// Read online: https://github.com/ocornut/imgui/tree/master/docs

// CHANGELOG
// (minor and older changes stripped away, please see git history for details)
//  2022-01-26: Inputs: replaced short-lived io.AddKeyModsEvent() (added two weeks ago)with io.AddKeyEvent() using ImGuiKey_ModXXX flags. Sorry for the confusion.
//  2022-01-17: Inputs: calling new io.AddMousePosEvent(), io.AddMouseButtonEvent(), io.AddMouseWheelEvent() API (1.87+).
//  2022-01-10: Inputs: calling new io.AddKeyEvent(), io.AddKeyModsEvent() + io.SetKeyEventNativeData() API (1.87+). Support for full ImGuiKey range.
//  2021-03-04: Initial version.

#include "imgui.h"
#include "imgui_impl_android.h"
#include <time.h>
#include <math.h>
#include <android/native_window.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>

// Android data
static double                                   g_Time = 0.0;
static ANativeWindow*                           g_Window;
static char                                     g_LogTag[] = "ImGuiExample";

// ── Touch input translation ─────────────────────────────────────────────
// Android delivers raw multi-pointer motion events; ImGui only knows
// mouse{pos,button,wheel}. After multiple iterations of clever gesture
// synthesis (deferred-MouseDown, time-gated slop, long-press → right
// click, etc.) all of which subtly broke widget interactions in the
// menu panels, we are back to the simplest design that actually works:
//
//   - Single finger DOWN  → MouseDown(0) at touch point
//   - Single finger MOVE  → MousePos at current point
//   - Single finger UP    → MouseUp(0)
//   - Second finger DOWN  → release MouseDown(0), enter PINCH mode
//   - Two-finger MOVE     → wheel ticks based on spread delta;
//                           position events suppressed
//   - Second finger UP    → exit PINCH; remaining finger does NOT
//                           re-press (waiting for full lift)
//   - Last finger UP      → reset state
//
// Tap-vs-drag discrimination is left ENTIRELY to ImGui's own logic
// (io.MouseDragThreshold). Click forgiveness for finger drift comes
// from a generous io.TouchExtraPadding set in style::applyTouchFriendly
// Tweaks(). No long-press → right-click on touch; right-click is for
// physical mice only. This trade — losing context-menu access via
// long-press — was made because the deferred MouseDown approach
// silently broke text inputs, slider activation, FreqSelect digits,
// and combo boxes throughout the menu tree. A working tap on every
// widget is more important than a long-press shortcut.
//
// State is intentionally global / single-context — there is exactly
// one ImGui context and one input thread on Android.
namespace {
    constexpr int   kMaxPointers       = 2;
    constexpr float kPinchDeadZonePx   = 12.0f;
    constexpr float kPinchScaleDivisor = 60.0f;  // raw px per wheel tick

    struct TouchPointer {
        int32_t id   = -1;        // -1 = slot empty
        float   curX = 0.f, curY = 0.f;
    };

    TouchPointer s_Pointers[kMaxPointers];
    bool         s_PinchActive       = false;
    bool         s_LeftDownEmitted   = false;  // MouseDown(0) currently held?
    float        s_LastPinchDistance = 0.f;

    inline int find_slot_by_id(int32_t id) {
        for (int i = 0; i < kMaxPointers; ++i)
            if (s_Pointers[i].id == id) return i;
        return -1;
    }
    inline int alloc_slot(int32_t id) {
        for (int i = 0; i < kMaxPointers; ++i)
            if (s_Pointers[i].id < 0) { s_Pointers[i].id = id; return i; }
        return -1;
    }
    inline int active_pointer_count() {
        int n = 0;
        for (int i = 0; i < kMaxPointers; ++i) if (s_Pointers[i].id >= 0) ++n;
        return n;
    }
    inline int primary_slot() {
        for (int i = 0; i < kMaxPointers; ++i) if (s_Pointers[i].id >= 0) return i;
        return -1;
    }
    inline float pointer_distance() {
        if (s_Pointers[0].id < 0 || s_Pointers[1].id < 0) return 0.f;
        float dx = s_Pointers[0].curX - s_Pointers[1].curX;
        float dy = s_Pointers[0].curY - s_Pointers[1].curY;
        return sqrtf(dx*dx + dy*dy);
    }
    inline void release_left_if_held(ImGuiIO& io) {
        if (s_LeftDownEmitted) {
            io.AddMouseButtonEvent(0, false);
            s_LeftDownEmitted = false;
        }
    }
    inline void clear_all_pointers() {
        for (int i = 0; i < kMaxPointers; ++i) s_Pointers[i] = TouchPointer{};
        s_PinchActive       = false;
        s_LastPinchDistance = 0.f;
    }
}

static ImGuiKey ImGui_ImplAndroid_KeyCodeToImGuiKey(int32_t key_code)
{
    switch (key_code)
    {
        case AKEYCODE_TAB:                  return ImGuiKey_Tab;
        case AKEYCODE_DPAD_LEFT:            return ImGuiKey_LeftArrow;
        case AKEYCODE_DPAD_RIGHT:           return ImGuiKey_RightArrow;
        case AKEYCODE_DPAD_UP:              return ImGuiKey_UpArrow;
        case AKEYCODE_DPAD_DOWN:            return ImGuiKey_DownArrow;
        case AKEYCODE_PAGE_UP:              return ImGuiKey_PageUp;
        case AKEYCODE_PAGE_DOWN:            return ImGuiKey_PageDown;
        case AKEYCODE_MOVE_HOME:            return ImGuiKey_Home;
        case AKEYCODE_MOVE_END:             return ImGuiKey_End;
        case AKEYCODE_INSERT:               return ImGuiKey_Insert;
        case AKEYCODE_FORWARD_DEL:          return ImGuiKey_Delete;
        case AKEYCODE_DEL:                  return ImGuiKey_Backspace;
        case AKEYCODE_SPACE:                return ImGuiKey_Space;
        case AKEYCODE_ENTER:                return ImGuiKey_Enter;
        case AKEYCODE_ESCAPE:               return ImGuiKey_Escape;
        case AKEYCODE_APOSTROPHE:           return ImGuiKey_Apostrophe;
        case AKEYCODE_COMMA:                return ImGuiKey_Comma;
        case AKEYCODE_MINUS:                return ImGuiKey_Minus;
        case AKEYCODE_PERIOD:               return ImGuiKey_Period;
        case AKEYCODE_SLASH:                return ImGuiKey_Slash;
        case AKEYCODE_SEMICOLON:            return ImGuiKey_Semicolon;
        case AKEYCODE_EQUALS:               return ImGuiKey_Equal;
        case AKEYCODE_LEFT_BRACKET:         return ImGuiKey_LeftBracket;
        case AKEYCODE_BACKSLASH:            return ImGuiKey_Backslash;
        case AKEYCODE_RIGHT_BRACKET:        return ImGuiKey_RightBracket;
        case AKEYCODE_GRAVE:                return ImGuiKey_GraveAccent;
        case AKEYCODE_CAPS_LOCK:            return ImGuiKey_CapsLock;
        case AKEYCODE_SCROLL_LOCK:          return ImGuiKey_ScrollLock;
        case AKEYCODE_NUM_LOCK:             return ImGuiKey_NumLock;
        case AKEYCODE_SYSRQ:                return ImGuiKey_PrintScreen;
        case AKEYCODE_BREAK:                return ImGuiKey_Pause;
        case AKEYCODE_NUMPAD_0:             return ImGuiKey_Keypad0;
        case AKEYCODE_NUMPAD_1:             return ImGuiKey_Keypad1;
        case AKEYCODE_NUMPAD_2:             return ImGuiKey_Keypad2;
        case AKEYCODE_NUMPAD_3:             return ImGuiKey_Keypad3;
        case AKEYCODE_NUMPAD_4:             return ImGuiKey_Keypad4;
        case AKEYCODE_NUMPAD_5:             return ImGuiKey_Keypad5;
        case AKEYCODE_NUMPAD_6:             return ImGuiKey_Keypad6;
        case AKEYCODE_NUMPAD_7:             return ImGuiKey_Keypad7;
        case AKEYCODE_NUMPAD_8:             return ImGuiKey_Keypad8;
        case AKEYCODE_NUMPAD_9:             return ImGuiKey_Keypad9;
        case AKEYCODE_NUMPAD_DOT:           return ImGuiKey_KeypadDecimal;
        case AKEYCODE_NUMPAD_DIVIDE:        return ImGuiKey_KeypadDivide;
        case AKEYCODE_NUMPAD_MULTIPLY:      return ImGuiKey_KeypadMultiply;
        case AKEYCODE_NUMPAD_SUBTRACT:      return ImGuiKey_KeypadSubtract;
        case AKEYCODE_NUMPAD_ADD:           return ImGuiKey_KeypadAdd;
        case AKEYCODE_NUMPAD_ENTER:         return ImGuiKey_KeypadEnter;
        case AKEYCODE_NUMPAD_EQUALS:        return ImGuiKey_KeypadEqual;
        case AKEYCODE_CTRL_LEFT:            return ImGuiKey_LeftCtrl;
        case AKEYCODE_SHIFT_LEFT:           return ImGuiKey_LeftShift;
        case AKEYCODE_ALT_LEFT:             return ImGuiKey_LeftAlt;
        case AKEYCODE_META_LEFT:            return ImGuiKey_LeftSuper;
        case AKEYCODE_CTRL_RIGHT:           return ImGuiKey_RightCtrl;
        case AKEYCODE_SHIFT_RIGHT:          return ImGuiKey_RightShift;
        case AKEYCODE_ALT_RIGHT:            return ImGuiKey_RightAlt;
        case AKEYCODE_META_RIGHT:           return ImGuiKey_RightSuper;
        case AKEYCODE_MENU:                 return ImGuiKey_Menu;
        case AKEYCODE_0:                    return ImGuiKey_0;
        case AKEYCODE_1:                    return ImGuiKey_1;
        case AKEYCODE_2:                    return ImGuiKey_2;
        case AKEYCODE_3:                    return ImGuiKey_3;
        case AKEYCODE_4:                    return ImGuiKey_4;
        case AKEYCODE_5:                    return ImGuiKey_5;
        case AKEYCODE_6:                    return ImGuiKey_6;
        case AKEYCODE_7:                    return ImGuiKey_7;
        case AKEYCODE_8:                    return ImGuiKey_8;
        case AKEYCODE_9:                    return ImGuiKey_9;
        case AKEYCODE_A:                    return ImGuiKey_A;
        case AKEYCODE_B:                    return ImGuiKey_B;
        case AKEYCODE_C:                    return ImGuiKey_C;
        case AKEYCODE_D:                    return ImGuiKey_D;
        case AKEYCODE_E:                    return ImGuiKey_E;
        case AKEYCODE_F:                    return ImGuiKey_F;
        case AKEYCODE_G:                    return ImGuiKey_G;
        case AKEYCODE_H:                    return ImGuiKey_H;
        case AKEYCODE_I:                    return ImGuiKey_I;
        case AKEYCODE_J:                    return ImGuiKey_J;
        case AKEYCODE_K:                    return ImGuiKey_K;
        case AKEYCODE_L:                    return ImGuiKey_L;
        case AKEYCODE_M:                    return ImGuiKey_M;
        case AKEYCODE_N:                    return ImGuiKey_N;
        case AKEYCODE_O:                    return ImGuiKey_O;
        case AKEYCODE_P:                    return ImGuiKey_P;
        case AKEYCODE_Q:                    return ImGuiKey_Q;
        case AKEYCODE_R:                    return ImGuiKey_R;
        case AKEYCODE_S:                    return ImGuiKey_S;
        case AKEYCODE_T:                    return ImGuiKey_T;
        case AKEYCODE_U:                    return ImGuiKey_U;
        case AKEYCODE_V:                    return ImGuiKey_V;
        case AKEYCODE_W:                    return ImGuiKey_W;
        case AKEYCODE_X:                    return ImGuiKey_X;
        case AKEYCODE_Y:                    return ImGuiKey_Y;
        case AKEYCODE_Z:                    return ImGuiKey_Z;
        case AKEYCODE_F1:                   return ImGuiKey_F1;
        case AKEYCODE_F2:                   return ImGuiKey_F2;
        case AKEYCODE_F3:                   return ImGuiKey_F3;
        case AKEYCODE_F4:                   return ImGuiKey_F4;
        case AKEYCODE_F5:                   return ImGuiKey_F5;
        case AKEYCODE_F6:                   return ImGuiKey_F6;
        case AKEYCODE_F7:                   return ImGuiKey_F7;
        case AKEYCODE_F8:                   return ImGuiKey_F8;
        case AKEYCODE_F9:                   return ImGuiKey_F9;
        case AKEYCODE_F10:                  return ImGuiKey_F10;
        case AKEYCODE_F11:                  return ImGuiKey_F11;
        case AKEYCODE_F12:                  return ImGuiKey_F12;
        default:                            return ImGuiKey_None;
    }
}

int32_t ImGui_ImplAndroid_HandleInputEvent(AInputEvent* input_event)
{
    ImGuiIO& io = ImGui::GetIO();
    int32_t event_type = AInputEvent_getType(input_event);
    switch (event_type)
    {
    case AINPUT_EVENT_TYPE_KEY:
    {
        int32_t event_key_code = AKeyEvent_getKeyCode(input_event);
        int32_t event_scan_code = AKeyEvent_getScanCode(input_event);
        int32_t event_action = AKeyEvent_getAction(input_event);
        int32_t event_meta_state = AKeyEvent_getMetaState(input_event);

        io.AddKeyEvent(ImGuiKey_ModCtrl,  (event_meta_state & AMETA_CTRL_ON)  != 0);
        io.AddKeyEvent(ImGuiKey_ModShift, (event_meta_state & AMETA_SHIFT_ON) != 0);
        io.AddKeyEvent(ImGuiKey_ModAlt,   (event_meta_state & AMETA_ALT_ON)   != 0);
        io.AddKeyEvent(ImGuiKey_ModSuper, (event_meta_state & AMETA_META_ON)  != 0);

        switch (event_action)
        {
        // FIXME: AKEY_EVENT_ACTION_DOWN and AKEY_EVENT_ACTION_UP occur at once as soon as a touch pointer
        // goes up from a key. We use a simple key event queue/ and process one event per key per frame in
        // ImGui_ImplAndroid_NewFrame()...or consider using IO queue, if suitable: https://github.com/ocornut/imgui/issues/2787
        case AKEY_EVENT_ACTION_DOWN:
        case AKEY_EVENT_ACTION_UP:
        {
            ImGuiKey key = ImGui_ImplAndroid_KeyCodeToImGuiKey(event_key_code);
            if (key != ImGuiKey_None && (event_action == AKEY_EVENT_ACTION_DOWN || event_action == AKEY_EVENT_ACTION_UP))
            {
                io.AddKeyEvent(key, event_action == AKEY_EVENT_ACTION_DOWN);
                io.SetKeyEventNativeData(key, event_key_code, event_scan_code);
            }

            break;
        }
        default:
            break;
        }
        break;
    }
    case AINPUT_EVENT_TYPE_MOTION:
    {
        int32_t event_action_full   = AMotionEvent_getAction(input_event);
        int32_t event_pointer_index = (event_action_full & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
        int32_t event_action        = event_action_full & AMOTION_EVENT_ACTION_MASK;
        int32_t toolType            = AMotionEvent_getToolType(input_event, event_pointer_index);

        // Physical mouse path: forward straight through (no synthesis).
        // Detected when the event uses the BUTTON_*/HOVER/SCROLL actions —
        // these are mouse-only and never come from a finger.
        const bool isPhysicalMouseEvent =
            (toolType == AMOTION_EVENT_TOOL_TYPE_MOUSE) &&
            (event_action == AMOTION_EVENT_ACTION_BUTTON_PRESS ||
             event_action == AMOTION_EVENT_ACTION_BUTTON_RELEASE ||
             event_action == AMOTION_EVENT_ACTION_HOVER_MOVE     ||
             event_action == AMOTION_EVENT_ACTION_SCROLL);
        if (isPhysicalMouseEvent) {
            switch (event_action) {
            case AMOTION_EVENT_ACTION_BUTTON_PRESS:
            case AMOTION_EVENT_ACTION_BUTTON_RELEASE: {
                int32_t button_state = AMotionEvent_getButtonState(input_event);
                io.AddMousePosEvent(AMotionEvent_getX(input_event, event_pointer_index),
                                    AMotionEvent_getY(input_event, event_pointer_index));
                io.AddMouseButtonEvent(0, (button_state & AMOTION_EVENT_BUTTON_PRIMARY)   != 0);
                io.AddMouseButtonEvent(1, (button_state & AMOTION_EVENT_BUTTON_SECONDARY) != 0);
                io.AddMouseButtonEvent(2, (button_state & AMOTION_EVENT_BUTTON_TERTIARY)  != 0);
                break;
            }
            case AMOTION_EVENT_ACTION_HOVER_MOVE:
                io.AddMousePosEvent(AMotionEvent_getX(input_event, event_pointer_index),
                                    AMotionEvent_getY(input_event, event_pointer_index));
                break;
            case AMOTION_EVENT_ACTION_SCROLL:
                io.AddMouseWheelEvent(
                    AMotionEvent_getAxisValue(input_event, AMOTION_EVENT_AXIS_HSCROLL, event_pointer_index),
                    AMotionEvent_getAxisValue(input_event, AMOTION_EVENT_AXIS_VSCROLL, event_pointer_index));
                break;
            }
            break; // out of MOTION case
        }

        // Touch / stylus / unknown path — straight passthrough + pinch.
        // See header comment above the namespace block for the design.
        const int32_t pointerId = AMotionEvent_getPointerId(input_event, event_pointer_index);
        const float   evX       = AMotionEvent_getX(input_event, event_pointer_index);
        const float   evY       = AMotionEvent_getY(input_event, event_pointer_index);

        switch (event_action) {

        case AMOTION_EVENT_ACTION_DOWN: {
            // First finger down: start fresh, emit MousePos + MouseDown(0)
            // immediately so widgets see a normal mouse press at the touch
            // point. ImGui's own click/drag/release semantics then take over.
            clear_all_pointers();
            int slot = alloc_slot(pointerId);
            if (slot < 0) break;
            s_Pointers[slot].curX = evX;
            s_Pointers[slot].curY = evY;
            io.AddMousePosEvent(evX, evY);
            io.AddMouseButtonEvent(0, true);
            s_LeftDownEmitted = true;
            break;
        }

        case AMOTION_EVENT_ACTION_POINTER_DOWN: {
            // Second (or later) finger down: enter pinch mode. Release any
            // synthesized left button so the active widget cleanly stops
            // tracking — pinch is a distinct gesture, not a continuation
            // of the first finger's press.
            int slot = alloc_slot(pointerId);
            if (slot < 0) break;
            s_Pointers[slot].curX = evX;
            s_Pointers[slot].curY = evY;
            if (active_pointer_count() >= 2) {
                release_left_if_held(io);
                s_PinchActive       = true;
                s_LastPinchDistance = pointer_distance();
            }
            break;
        }

        case AMOTION_EVENT_ACTION_MOVE: {
            // Update curX/curY for every active pointer first (used by
            // pinch distance below).
            const int sampleCount = AMotionEvent_getPointerCount(input_event);
            for (int i = 0; i < sampleCount; ++i) {
                int32_t pid = AMotionEvent_getPointerId(input_event, i);
                int slot = find_slot_by_id(pid);
                if (slot < 0) continue;
                s_Pointers[slot].curX = AMotionEvent_getX(input_event, i);
                s_Pointers[slot].curY = AMotionEvent_getY(input_event, i);
            }

            if (s_PinchActive && active_pointer_count() >= 2) {
                // Pinch: emit wheel ticks based on spread delta. Mouse
                // position is intentionally NOT updated so ImGui doesn't
                // read the centroid drift as a single-finger swipe.
                float dist  = pointer_distance();
                float delta = dist - s_LastPinchDistance;
                if (fabsf(delta) > kPinchDeadZonePx) {
                    io.AddMouseWheelEvent(0.0f, delta / kPinchScaleDivisor);
                    s_LastPinchDistance = dist;
                }
            } else if (active_pointer_count() == 1 && s_LeftDownEmitted) {
                // Single-finger drag: forward position to ImGui. ImGui's
                // own MouseDragThreshold (set generously in
                // style::applyTouchFriendlyTweaks) decides whether this
                // becomes a drag or stays a click.
                int slot = primary_slot();
                if (slot >= 0) {
                    io.AddMousePosEvent(s_Pointers[slot].curX,
                                        s_Pointers[slot].curY);
                }
            }
            break;
        }

        case AMOTION_EVENT_ACTION_POINTER_UP: {
            // One of the secondary fingers lifted. If we drop below 2
            // fingers, exit pinch. We do NOT re-press the left button for
            // the remaining finger — the user has to fully lift and tap
            // again to start a new gesture.
            int slot = find_slot_by_id(pointerId);
            if (slot >= 0) s_Pointers[slot] = TouchPointer{};
            if (active_pointer_count() < 2) {
                s_PinchActive       = false;
                s_LastPinchDistance = 0.f;
            }
            break;
        }

        case AMOTION_EVENT_ACTION_UP: {
            // Last finger up: release the left button if we still hold it.
            // ImGui will fire the click event on this release if the
            // cursor is still inside the original widget's hit-test area
            // (extended by io.TouchExtraPadding for finger-drift forgiveness).
            release_left_if_held(io);
            clear_all_pointers();
            break;
        }

        case AMOTION_EVENT_ACTION_CANCEL: {
            release_left_if_held(io);
            clear_all_pointers();
            break;
        }

        default:
            break;
        }
        break;
    }
        return 1;
    default:
        break;
    }

    return 0;
}

bool ImGui_ImplAndroid_Init(ANativeWindow* window)
{
    g_Window = window;
    g_Time = 0.0;

    // Setup backend capabilities flags
    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = "imgui_impl_android";

    return true;
}

void ImGui_ImplAndroid_Shutdown()
{
}

void ImGui_ImplAndroid_NewFrame()
{
    ImGuiIO& io = ImGui::GetIO();

    // Setup display size (every frame to accommodate for window resizing)
    int32_t window_width = ANativeWindow_getWidth(g_Window);
    int32_t window_height = ANativeWindow_getHeight(g_Window);
    int display_width = window_width;
    int display_height = window_height;

    io.DisplaySize = ImVec2((float)window_width, (float)window_height);
    if (window_width > 0 && window_height > 0)
        io.DisplayFramebufferScale = ImVec2((float)display_width / window_width, (float)display_height / window_height);

    // Setup time step
    struct timespec current_timespec;
    clock_gettime(CLOCK_MONOTONIC, &current_timespec);
    double current_time = (double)(current_timespec.tv_sec) + (current_timespec.tv_nsec / 1000000000.0);
    io.DeltaTime = g_Time > 0.0 ? (float)(current_time - g_Time) : (float)(1.0f / 60.0f);
    g_Time = current_time;
}
