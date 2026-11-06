#include "app/app.h"
#include "app/overlay.h"
#include "build_info.h"  // MECHASSAULT_DESCRIBE, written at every build (cmake/build_info.cmake)
#include "core/game_import.h"
#include "core/input.h"

#include <imgui.h>

namespace mechassault {
void KeyboardPad(int port, input::Pad& pad);
void LoadBindings(const void* table);
void SaveBindings(void* table);
void BindingsMenu();
void ScriptPad(int port, input::Pad& pad);
void MouseAimPad(int port, input::Pad& pad);
void MouseWindow(void* hwnd);
bool MouseMessage(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
void MouseMenu();
extern bool g_mouse_aim;
extern uint32_t g_mouse_sensitivity;
const gameimport::Profile& Profile();  // src/common/profile.cpp
extern bool g_mouse_invert_y;
extern uint32_t g_fov_percent;  // src/game/widescreen.cpp
extern bool g_unlock_campaign;  // src/game/campaign.cpp
void UpdatePrompts();          // src/game/prompts.cpp
}

namespace {

// the F1 menu's game tabs: Controls, then Gameplay (field of view, language, the campaign unlock)
void GameMenu() {
    mechassault::MouseMenu();
    if (!app::MenuPage("Gameplay")) return;
    int fov = int(mechassault::g_fov_percent);
    app::MenuRow("Field of view");
    if (ImGui::SliderInt("##fov", &fov, 80, 130, "%d%%")) {
        mechassault::g_fov_percent = uint32_t(fov);
        app::Save();
    }
    app::MenuHint("Of the game's own view, for every camera including cutscenes; 100% is the original.");
    // the console language the game is told; it reads it as it starts
    static const std::pair<const char*, uint32_t> kLanguages[] = {{"English", 1}, {"Franç" "ais", 4}, {"Deutsch", 3}};
    int lang = 0;
    for (int i = 0; i < 3; i++)
        if (kLanguages[i].second == app::Get().language) lang = i;
    const char* names[] = {kLanguages[0].first, kLanguages[1].first, kLanguages[2].first};
    app::MenuRow("Language");
    if (ImGui::Combo("##language", &lang, names, 3)) {
        app::Get().language = kLanguages[lang].second;
        app::Save();
    }
    app::MenuHint("Text and speech, from the next time the game starts.");
    app::MenuRow("Unlock whole campaign");
    if (ImGui::Checkbox("##unlock", &mechassault::g_unlock_campaign)) app::Save();
    app::MenuHint("Every mission and movie in the campaign's Select Mission, from the next visit to it.");
    app::EndMenuPage();
}

// keyboard and mouse buttons (rebindable), then a test script's presses when XBE_PAD_SCRIPT is set
void Pad(int port, input::Pad& pad) {
    if (port == 0) mechassault::UpdatePrompts();
    mechassault::ScriptPad(port, pad);
    mechassault::KeyboardPad(port, pad);
    mechassault::MouseAimPad(port, pad);
}

}  // namespace

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"MechAssault",
    .short_title = L"MechAssault",
    .name = L"MechAssault",
    .log = "mechassault",
    .settings = "settings.toml",
    .splash = L"splash",
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its files beside it.",
    .images = {{"default.xbe", "MechAssault"}},
    .options = {{"mouse_aim", &mechassault::g_mouse_aim, nullptr},  // game settings kept in the settings file
                {"mouse_sensitivity", nullptr, &mechassault::g_mouse_sensitivity},  // percent, 100 = 0.05 degrees a count
                {"mouse_invert_y", &mechassault::g_mouse_invert_y, nullptr},
                {"fov_percent", nullptr, &mechassault::g_fov_percent},  // field of view, 80-130
                {"unlock_campaign", &mechassault::g_unlock_campaign, nullptr}},
    .load = mechassault::LoadBindings,  // [bindings]: keys and mouse buttons (src/common/keyboard_pad.cpp)
    .save = mechassault::SaveBindings,
    .init = nullptr,
    .loaded = nullptr,
    .window = mechassault::MouseWindow,  // raw mouse input (src/common/mouse_aim.cpp)
    .message = mechassault::MouseMessage,
    .ui = nullptr,
    .menu = GameMenu,
    .pad = Pad,  // keyboard and mouse buttons as pad 1, rebindable; XBE_PAD_SCRIPT for test runs
    .rumble = nullptr,
    .profile = &mechassault::Profile(),  // src/common/profile.cpp
    .extras = {{L"Mouse aim", L"Turn your 'Mech and aim with the mouse. The keyboard and a controller always work too.",
                &mechassault::g_mouse_aim},
               {L"Unlock the whole campaign", L"Every mission and movie playable from the mission select at once.",
                &mechassault::g_unlock_campaign},
               {L"Online games", L"Host and join System Link games over the internet, in the game's own menu.",
                &app::Get().online_games}},
    .languages = {{L"English", 1}, {L"Français", 4}, {L"Deutsch", 3}},  // the disc's (En, Fr, De)
    .controls = L"Mouse aim · WASD move · Left click fire · Right click or Q cycle weapons · "
                L"1 2 3 energy, ballistic, missile · Space jump jets · Shift defense · Esc pause",
    .platform_line = L"Original Xbox · Title ID 4D530017",
    .version = MECHASSAULT_DESCRIBE,
    .update_repo = "CaiusCodes/mechassault-xbox-pc-recomp",  // its releases carry MechAssault.zip (app/update.h)
};
