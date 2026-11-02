#include "app/app.h"
#include "core/game_import.h"
#include "core/input.h"

namespace mechassault {
void KeyboardPad(int port, input::Pad& pad);
void LoadBindings(const void* table);
void SaveBindings(void* table);
void BindingsMenu();
}

namespace {

// the setup screen's check of the player's copy: MechAssault (USA, Europe), the release this build was recompiled from
const gameimport::Profile kProfile = [] {
    gameimport::Profile p;
    p.name = "MechAssault";
    p.title_id = 0x4D530017;  // "MS" 023
    p.xbe_sha1 = {"682ddd1ecda1e5110f9339dab0c31d296d51777b"};
    p.required = {"default.xbe", "Common.mgf", "text.mgf", "movies.mgf", "Audio/Music.d1Bank", "Audio/SFX.d1Bank",
                  "Audio/SFX_English.d1Bank", "media/ArialUni.Ttf", "media/dlcustom.xpr", "Movie/NTSC/MAIntro.bik"};
    for (const char* m : {"01", "02", "03", "04", "05", "06a", "06b", "07a", "07b", "09", "10", "12", "13", "15a",
                          "15b", "17", "18", "19", "20a", "20B"})
        p.required.push_back(std::string("Mission") + m + ".mgf");
    p.required_dirs = {"Audio", "media", "Movie/NTSC", "Movie/PAL"};
    return p;
}();

void GameMenu() { mechassault::BindingsMenu(); }

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
    .options = {},
    .load = mechassault::LoadBindings,  // [bindings]: keys and mouse buttons (src/common/keyboard_pad.cpp)
    .save = mechassault::SaveBindings,
    .init = nullptr,
    .loaded = nullptr,
    .window = nullptr,
    .message = nullptr,
    .ui = nullptr,
    .menu = GameMenu,
    .pad = mechassault::KeyboardPad,  // keyboard and mouse buttons as pad 1, rebindable
    .rumble = nullptr,
    .profile = &kProfile,
    .extras = {},
    .controls = L"Keyboard: WASD move · IJKL or arrows look · Enter Start · Space A · rebind in F1",
    .platform_line = L"Original Xbox · Title ID 4D530017",
};
