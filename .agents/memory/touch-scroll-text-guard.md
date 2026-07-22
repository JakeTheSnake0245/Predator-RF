---
name: Touch scroll vs text fields
description: How the Android panel-scroll steal coexists with ImGui text inputs
---
Rule: never gate touch panel scrolling on `io.WantTextInput` alone — in field-dense menus every press lands on an input and scrolling dies entirely.

**Why:** field report — hits/config menus with many InputText/pendEdit fields could not be scrolled; every touch was eaten by the field under the finger.

**How to apply:** yield to the text widget only when the operator was already typing BEFORE the press (previous-frame WantTextInput captured at IsMouseClicked). Tap = activate field; hold+slide = scroll (slop steal clears ActiveId). The previous-frame tracker must update before any `!MouseDown` early-return, or it goes stale between gestures and suppresses later drags.

Related: SIGSEGV at fault offset ~0x28 in ImGui_ImplOpenGL3_RenderDrawData means the GL backend data was null (surface torn down mid-frame on Android). Guards exist in the Android backend render loop; crash logs showing this from an APK path different from the current install are from a pre-guard build.
