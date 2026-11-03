#pragma once
// breakdown-recomp patch: the PC overlay (overlay.cpp)
namespace app {
void DrawOverlay();  // each frame inside the ImGui frame, UiLock held, before GameInfo::ui
void ToggleMenu();   // window thread: F1
// mechassault-recomp patch: the F1 menu's pieces for the game's own tabs (GameInfo::menu draws them): a page (a tab;
// when true, fill it and call EndMenuPage), a setting's label in the left column (the next control fills the right),
// a hover note for the control just drawn
bool MenuPage(const char* name);
void EndMenuPage();
void MenuRow(const char* label);
void MenuHint(const char* text);
}  // namespace app
