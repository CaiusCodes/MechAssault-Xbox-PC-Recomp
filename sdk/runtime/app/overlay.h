#pragma once
// breakdown-recomp patch: the PC overlay (overlay.cpp)
namespace app {
void DrawOverlay();  // each frame inside the ImGui frame, UiLock held, before GameInfo::ui
void ToggleMenu();   // window thread: F1
}  // namespace app
