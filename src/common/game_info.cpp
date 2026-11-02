#include "app/app.h"
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
}

namespace {

// the F1 menu's game sections: Gameplay (field of view), then Controls
void GameMenu() {
    if (ImGui::CollapsingHeader("Gameplay", ImGuiTreeNodeFlags_DefaultOpen)) {
        int fov = int(mechassault::g_fov_percent);
        if (ImGui::SliderInt("Field of view", &fov, 80, 130, "%d%%")) {
            mechassault::g_fov_percent = uint32_t(fov);
            app::Save();
        }
        ImGui::TextDisabled("Of the game's own view, for every camera including cutscenes; 100%% is the original.");
    }
    mechassault::MouseMenu();
}

// keyboard and mouse buttons (rebindable), then a test script's presses when XBE_PAD_SCRIPT is set
void Pad(int port, input::Pad& pad) {
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
                {"fov_percent", nullptr, &mechassault::g_fov_percent}},  // field of view, 80-130
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
    .extras = {{L"Mouse aim", L"Turn the torso and aim with the mouse. The keyboard and a controller always work too.",
                &mechassault::g_mouse_aim}},
    .controls = L"Mouse aim · WASD move · Left click fire · Right click or Q cycle weapons · "
                L"1 2 3 energy, ballistic, missile · Space jump jets · Shift defense · Esc pause",
    .platform_line = L"Original Xbox · Title ID 4D530017",
};
