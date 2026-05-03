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

// ── Touch gesture synthesis ─────────────────────────────────────────────
// Android delivers raw multi-pointer motion events; ImGui only knows
// mouse{pos,button,wheel}. The stock imgui_impl_android binding mapped
// every ACTION_DOWN to MouseButton(0, true), which on a touch-first
// tactical UI broke three things:
//
//   1. No long-press → right-click. ImGui's context-menu hooks (waterfall
//      freq mark, module config) are right-click-only and were unreachable.
//
//   2. No multi-touch. Two fingers on the waterfall registered as a
//      single-finger drag tracking the centroid, not a pinch-to-zoom.
//
//   3. Every tap fired a "MouseDown" event before the tap was confirmed,
//      which (combined with the original tiny MouseDragThreshold) re-
//      classified taps as drags and stole them from buttons/tabs.
//
// This translator fixes all three by deferring the mouse-button event
// until the gesture is committed:
//
//   - Tap   (UP within 500 ms, no move > slop) → DOWN+UP at down position
//   - Drag  (move > slop while down)           → DOWN at down position,
//                                                MOVE to current
//   - Long  (>= 500 ms with no move > slop)    → right-click DOWN+UP at
//                                                down position
//   - Pinch (≥ 2 fingers down)                 → wheel ticks based on
//                                                spread delta; suppress
//                                                cursor pos updates
//
// State is intentionally global / single-context — there is exactly one
// ImGui context and one input thread on Android.
namespace {
    constexpr int    kMaxPointers       = 2;
    // Floor for tap-vs-drag slop in raw pixels. The actual slop used at
    // runtime is max(this, ImGui::GetIO().MouseDragThreshold) so we stay
    // in lockstep with style::applyTouchFriendlyTweaks() which sets the
    // ImGui drag threshold to 20*uiScale (≈60 px on Android). Without
    // syncing, our synthesizer commits a "drag" at 24 px while ImGui
    // itself would still call it a click — so a tap inside a sub-menu
    // panel gets eaten by the parent window's scroll handler before the
    // child widget ever sees a click.
    constexpr float  kTapSlopFloorPx    = 24.0f;
    constexpr float  kPinchDeadZonePx   = 12.0f;
    constexpr float  kPinchScaleDivisor = 60.0f;   // raw px per wheel tick
    constexpr double kLongPressSec      = 0.5;

    struct TouchPointer {
        int32_t id   = -1;          // -1 = slot empty
        float   downX = 0.f, downY = 0.f;
        float   curX  = 0.f, curY  = 0.f;
        double  downTime       = 0.0;
        bool    movedPastSlop  = false;
        bool    leftDownEmitted = false;
        bool    longPressFired  = false;
    };

    TouchPointer s_Pointers[kMaxPointers];
    bool         s_PinchActive       = false;
    float        s_LastPinchDistance = 0.f;

    inline double monotonic_now_sec() {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    }
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
    inline void clear_all_pointers() {
        for (int i = 0; i < kMaxPointers; ++i) s_Pointers[i] = TouchPointer{};
        s_PinchActive       = false;
        s_LastPinchDistance = 0.f;
    }
    inline float effective_tap_slop_px() {
        float t = ImGui::GetIO().MouseDragThreshold;
        return (t > kTapSlopFloorPx) ? t : kTapSlopFloorPx;
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

        // Touch / stylus / unknown path — gesture synthesis (see notes
        // above the TouchPointer state at the top of this file).
        const int32_t pointerId = AMotionEvent_getPointerId(input_event, event_pointer_index);
        const float   evX       = AMotionEvent_getX(input_event, event_pointer_index);
        const float   evY       = AMotionEvent_getY(input_event, event_pointer_index);

        switch (event_action) {

        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN: {
            if (event_action == AMOTION_EVENT_ACTION_DOWN) clear_all_pointers();
            int slot = alloc_slot(pointerId);
            if (slot < 0) break;             // > kMaxPointers, drop extra
            s_Pointers[slot].downX = s_Pointers[slot].curX = evX;
            s_Pointers[slot].downY = s_Pointers[slot].curY = evY;
            s_Pointers[slot].downTime        = monotonic_now_sec();
            s_Pointers[slot].movedPastSlop   = false;
            s_Pointers[slot].leftDownEmitted = false;
            s_Pointers[slot].longPressFired  = false;

            const int n = active_pointer_count();
            if (n >= 2) {
                // Second finger landed: cancel any pending single-finger
                // gesture, enter pinch mode. We mark the first slot's
                // long-press as "fired" so the tick handler doesn't
                // promote it to right-click while we're pinching.
                for (int i = 0; i < kMaxPointers; ++i) {
                    if (s_Pointers[i].id < 0) continue;
                    if (s_Pointers[i].leftDownEmitted) {
                        // Drag had already been committed — release it.
                        io.AddMouseButtonEvent(0, false);
                        s_Pointers[i].leftDownEmitted = false;
                    }
                    s_Pointers[i].longPressFired = true;
                }
                s_PinchActive       = true;
                s_LastPinchDistance = pointer_distance();
            }
            break;
        }

        case AMOTION_EVENT_ACTION_MOVE: {
            // Slop is dynamic — see kTapSlopFloorPx note. Sampled once per
            // event so all pointers in the same MotionEvent see the same
            // threshold.
            const float slop = effective_tap_slop_px();
            const float slopSq = slop * slop;
            const int sampleCount = AMotionEvent_getPointerCount(input_event);
            for (int i = 0; i < sampleCount; ++i) {
                int32_t pid = AMotionEvent_getPointerId(input_event, i);
                int slot = find_slot_by_id(pid);
                if (slot < 0) continue;
                float x = AMotionEvent_getX(input_event, i);
                float y = AMotionEvent_getY(input_event, i);
                s_Pointers[slot].curX = x;
                s_Pointers[slot].curY = y;
                float dx = x - s_Pointers[slot].downX;
                float dy = y - s_Pointers[slot].downY;
                if (dx*dx + dy*dy > slopSq)
                    s_Pointers[slot].movedPastSlop = true;
            }

            const int n = active_pointer_count();
            if (n >= 2 && s_PinchActive) {
                // Pinch / two-finger scroll: emit wheel ticks based on
                // spread delta. Mouse position is intentionally NOT
                // updated so ImGui doesn't read the centroid drift as
                // a single-finger swipe across the whole UI.
                float dist  = pointer_distance();
                float delta = dist - s_LastPinchDistance;
                if (fabsf(delta) > kPinchDeadZonePx) {
                    io.AddMouseWheelEvent(0.0f, delta / kPinchScaleDivisor);
                    s_LastPinchDistance = dist;
                }
            } else if (n == 1) {
                int slot = primary_slot();
                if (slot < 0) break;
                // Commit the deferred MouseButton(0, true) the moment the
                // gesture clearly becomes a drag. Emit at the ORIGINAL
                // down position so ImGui's drag-start point is correct.
                if (!s_Pointers[slot].leftDownEmitted &&
                    !s_Pointers[slot].longPressFired  &&
                    s_Pointers[slot].movedPastSlop) {
                    io.AddMousePosEvent(s_Pointers[slot].downX, s_Pointers[slot].downY);
                    io.AddMouseButtonEvent(0, true);
                    s_Pointers[slot].leftDownEmitted = true;
                }
                if (s_Pointers[slot].leftDownEmitted) {
                    io.AddMousePosEvent(s_Pointers[slot].curX, s_Pointers[slot].curY);
                }
            }
            break;
        }

        case AMOTION_EVENT_ACTION_POINTER_UP: {
            int slot = find_slot_by_id(pointerId);
            if (slot >= 0) s_Pointers[slot] = TouchPointer{};
            // Exit pinch mode but treat any remaining finger as already-
            // committed (no tap, no long-press) so we don't emit a stray
            // click when the user lifts the second finger.
            s_PinchActive = false;
            int p = primary_slot();
            if (p >= 0) {
                s_Pointers[p].longPressFired = true;
                io.AddMousePosEvent(s_Pointers[p].curX, s_Pointers[p].curY);
            }
            break;
        }

        case AMOTION_EVENT_ACTION_UP: {
            // Last finger up. Three cases:
            //   - leftDownEmitted: drag, just release the button
            //   - long-press already fired: nothing to emit (right-click
            //     was its own DOWN+UP)
            //   - otherwise: real tap → emit DOWN+UP at the down position
            int slot = find_slot_by_id(pointerId);
            if (slot < 0) slot = primary_slot();
            if (slot >= 0) {
                if (s_Pointers[slot].leftDownEmitted) {
                    io.AddMouseButtonEvent(0, false);
                } else if (!s_Pointers[slot].longPressFired &&
                           !s_Pointers[slot].movedPastSlop) {
                    io.AddMousePosEvent(s_Pointers[slot].downX, s_Pointers[slot].downY);
                    io.AddMouseButtonEvent(0, true);
                    io.AddMouseButtonEvent(0, false);
                }
            }
            clear_all_pointers();
            break;
        }

        case AMOTION_EVENT_ACTION_CANCEL: {
            for (int i = 0; i < kMaxPointers; ++i)
                if (s_Pointers[i].leftDownEmitted) io.AddMouseButtonEvent(0, false);
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

// Long-press tick. Called every frame from NewFrame. If a single finger
// has been held still for ≥ kLongPressSec without committing a drag, we
// synthesize a right-click at the down position. Marks the slot as
// long-press-fired so ACTION_UP becomes a no-op for that gesture.
static void ImGui_ImplAndroid_TickLongPress()
{
    if (active_pointer_count() != 1) return;
    int slot = primary_slot();
    if (slot < 0) return;
    TouchPointer& p = s_Pointers[slot];
    if (p.movedPastSlop || p.leftDownEmitted || p.longPressFired) return;
    if (monotonic_now_sec() - p.downTime < kLongPressSec) return;

    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(p.downX, p.downY);
    io.AddMouseButtonEvent(1, true);
    io.AddMouseButtonEvent(1, false);
    p.longPressFired = true;
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

    // Promote idle single-finger holds to right-click.
    ImGui_ImplAndroid_TickLongPress();
}
