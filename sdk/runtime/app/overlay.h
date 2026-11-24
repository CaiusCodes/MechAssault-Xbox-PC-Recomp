#pragma once
// breakdown-recomp patch: the PC overlay (overlay.cpp)
#include <cstdint>

namespace app {
void DrawOverlay();  // each frame inside the ImGui frame, UiLock held, before GameInfo::ui
void ToggleMenu();   // window thread: F1
void LoadMenuFont();  // mechassault-recomp patch: once, after ImGui::CreateContext (Bahnschrift, MA_MENU_FONT)

// mechassault-recomp patch: the F1 menu's rows (overlay.cpp), for the runtime's pages and the game's (GameInfo::menu
// is called on each page; MenuPage tells which). A page is a two-column table: names stretching, controls in a fixed
// column. Row starts a row (the next control fills the right column), Hint gives the control just drawn its
// description (shown under the page while its row is hovered or selected), Heading a muted line between groups,
// Rows(false) ends the table for a page's own layout (Rows(true) starts the next). Every change applies at once and
// calls app::Save().
void Row(const char* label);
void Hint(const char* text);
void Heading(const char* text);
void Rows(bool on);
bool Slider(const char* id, uint32_t& value, int lo, int hi, const char* format);  // its value text right of the track
bool Toggle(const char* id, bool& value);                                           // a pill switch
// mechassault-recomp patch: a < name > picker over names (the arrows, left and right step it; A or a click opens
// the list); index is the chosen name's
bool Pick(const char* id, int& index, const char* const* names, int count);
// the game's page: true on that page ("Display", "Graphics", "Game" or "Gameplay", "Controls", "Network"), its rows
// open; EndMenuPage closes nothing now and is kept for the game's code
bool MenuPage(const char* name);
void EndMenuPage();
void MenuRow(const char* label);  // Row
void MenuHint(const char* text);  // Hint
}  // namespace app
