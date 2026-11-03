// controllers through SDL: Xbox, PlayStation and Switch pads read into the console controller's report
#include "core/input.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <mutex>
#include <utility>

#include <SDL3/SDL.h>

#include "app/app.h"
#include "core/log.h"

namespace input {

namespace {

// a controller takes the lowest free port when it is plugged in and keeps it until it is unplugged
std::mutex g_lock;
bool g_sdl = false;
SDL_Gamepad* g_pads[4] = {};

void AddPad(SDL_JoystickID id) {
    for (SDL_Gamepad* pad : g_pads)
        if (pad && SDL_GetGamepadID(pad) == id) return;
    SDL_Gamepad** slot = std::find(std::begin(g_pads), std::end(g_pads), nullptr);
    if (slot == std::end(g_pads)) return;
    *slot = SDL_OpenGamepad(id);
    const char* name = *slot ? SDL_GetGamepadName(*slot) : SDL_GetError();
    LOGI("input: controller %d %s: %s", int(slot - g_pads) + 1, *slot ? "plugged in" : "failed to open", name ? name : "");
}

void RemovePad(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++) {
        if (!g_pads[i] || SDL_GetGamepadID(g_pads[i]) != id) continue;
        SDL_CloseGamepad(g_pads[i]);
        g_pads[i] = nullptr;
        LOGI("input: controller %d unplugged", i + 1);
    }
}

// once a millisecond at most, however many ports and threads read
void UpdatePads() {
    static uint64_t last = 0;
    uint64_t now = SDL_GetTicks();
    if (!g_sdl || now == last) return;
    last = now;
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) RemovePad(e.gdevice.which);
    }
}

// breakdown-recomp patch: Back and Start together open the PC menu. Read under g_lock by the game's polls and the
// menu's, whichever comes first, so the game never sees that Start.
bool g_combo_down = false;     // Back and Start both held on some controller
bool g_combo_pending = false;  // pressed, not yet read by the menu
bool g_combo_latch = false;    // kept from the game until Back and Start are both let go
bool g_menu_was_open = false;  // the menu closed: buttons kept from the game until all are let go

void UpdateCombo() {
    bool both = false, either = false;
    for (SDL_Gamepad* pad : g_pads) {
        if (!pad) continue;
        bool start = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
        bool back = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK);
        both |= start && back;
        either |= start || back;
    }
    if (both && !g_combo_down) {
        g_combo_pending = true;
        g_combo_latch = true;
    }
    g_combo_down = both;
    if (!either) g_combo_latch = false;
}

}  // namespace

void Init() {
    if (g_sdl) return;  // started already for the game code window
    // WGI reads the triggers apart and HIDAPI takes PlayStation and Switch pads. Reads come from the guest's threads,
    // so SDL's own thread takes the device notices.
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        LOGE("input: SDL failed to start, no controllers: %s", SDL_GetError());
        return;
    }
    int v = SDL_GetVersion();
    LOGI("input: SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    std::lock_guard lock(g_lock);
    g_sdl = true;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; i++) AddPad(ids[i]);
        SDL_free(ids);
    }
}

// mechassault-recomp patch: a controller is plugged in (the button prompts follow it)
bool AnyController() {
    std::lock_guard lock(g_lock);
    return std::any_of(std::begin(g_pads), std::end(g_pads), [](SDL_Gamepad* p) { return p != nullptr; });
}

Pad Poll(int port) {
    Pad p;
    {
        std::lock_guard lock(g_lock);
        UpdatePads();
        UpdateCombo();
        if (SDL_Gamepad* pad = port >= 0 && port < 4 ? g_pads[port] : nullptr) {
            p.connected = true;
            constexpr SDL_GamepadButton kButtons[] = {
                SDL_GAMEPAD_BUTTON_DPAD_UP,   SDL_GAMEPAD_BUTTON_DPAD_DOWN,  SDL_GAMEPAD_BUTTON_DPAD_LEFT,
                SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,
                SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
            for (int i = 0; i < 8; i++) p.buttons |= uint16_t(SDL_GetGamepadButton(pad, kButtons[i]) << i);
            // A B X Y black white
            constexpr SDL_GamepadButton kAnalog[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                                     SDL_GAMEPAD_BUTTON_WEST,  SDL_GAMEPAD_BUTTON_NORTH,
                                                     SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
            for (int i = 0; i < 6; i++) p.analog[i] = SDL_GetGamepadButton(pad, kAnalog[i]) ? 0xFF : 0;
            auto axis = [&](SDL_GamepadAxis a) { return int(SDL_GetGamepadAxis(pad, a)); };
            // 0 to 32767 into 0 to 255
            p.analog[6] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0) >> 7);
            p.analog[7] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0) >> 7);
            // SDL's sticks have y down
            // not plain negation: -32768 has no positive
            p.lx = int16_t(axis(SDL_GAMEPAD_AXIS_LEFTX));
            p.ly = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_LEFTY));
            p.rx = int16_t(axis(SDL_GAMEPAD_AXIS_RIGHTX));
            p.ry = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_RIGHTY));
            if (g_combo_latch) p.buttons &= uint16_t(~0x30);  // Start and Back
        }
    }
    if (app::kGame.pad) app::kGame.pad(port, p);
    // breakdown-recomp patch: the PC settings menu has the controls; the pad stays connected, idle. Once it closes,
    // the buttons that were held for it (B, Start, Enter) stay away from the game until they are let go.
    bool open = app::MenuOpen();
    bool held = (p.buttons & 0xF0) ||  // Start, Back, the stick clicks; A B X Y black white
                std::any_of(std::begin(p.analog), std::begin(p.analog) + 6, [](uint8_t a) { return a > 0x20; });
    {
        std::lock_guard lock(g_lock);
        if (open) g_menu_was_open = true;
        else if (g_menu_was_open && !held) g_menu_was_open = false;
        open = open || g_menu_was_open;
    }
    if (open) {
        Pad idle;
        idle.connected = p.connected;
        if (!app::MenuOpen()) {  // just closed: the sticks and d-pad go through, the buttons wait
            idle.lx = p.lx, idle.ly = p.ly, idle.rx = p.rx, idle.ry = p.ry;
            idle.buttons = p.buttons & 0x0F;
        }
        p = idle;
    }
    return p;
}

MenuPad ReadMenuPad() {
    MenuPad m;
    std::lock_guard lock(g_lock);
    UpdatePads();
    UpdateCombo();
    m.toggle = g_combo_pending;
    g_combo_pending = false;
    constexpr std::pair<SDL_GamepadButton, uint32_t> kButtons[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, kMenuA},         {SDL_GAMEPAD_BUTTON_EAST, kMenuB},
        {SDL_GAMEPAD_BUTTON_WEST, kMenuX},          {SDL_GAMEPAD_BUTTON_NORTH, kMenuY},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, kMenuUp},      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kMenuDown},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kMenuLeft},  {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kMenuRight},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kMenuLB}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kMenuRB}};
    for (SDL_Gamepad* pad : g_pads) {
        if (!pad) continue;
        for (auto [button, bit] : kButtons)
            if (SDL_GetGamepadButton(pad, button)) m.buttons |= bit;
        float lx = float(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX)) / 32767.0f;
        float ly = -float(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY)) / 32767.0f;
        if (std::fabs(lx) > std::fabs(m.lx)) m.lx = std::clamp(lx, -1.0f, 1.0f);
        if (std::fabs(ly) > std::fabs(m.ly)) m.ly = std::clamp(ly, -1.0f, 1.0f);
    }
    return m;
}

uint16_t HeldOnAnyController() {
    constexpr std::pair<SDL_GamepadButton, uint16_t> kFaceButtons[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
        {SDL_GAMEPAD_BUTTON_WEST, 0x4000},  {SDL_GAMEPAD_BUTTON_NORTH, 0x8000}};
    std::lock_guard lock(g_lock);
    UpdatePads();
    uint16_t held = 0;
    for (SDL_Gamepad* pad : g_pads)
        for (auto [button, mask] : kFaceButtons)
            if (pad && SDL_GetGamepadButton(pad, button)) held |= mask;
    return held;
}

// the motors run until set again
void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    std::lock_guard lock(g_lock);
    if (port >= 0 && port < 4 && g_pads[port]) SDL_RumbleGamepad(g_pads[port], left, right, 0);
}

}  // namespace input
