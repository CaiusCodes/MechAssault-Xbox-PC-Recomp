// mechassault-recomp (from the Breakdown port): keyboard and mouse buttons as controller port 1, rebindable in the F1
// menu's Controls section. Each control of the Xbox pad (buttons, triggers, d-pad, stick directions) takes up to two
// keys or mouse buttons, merged with a real pad on that port. The defaults follow MechAssault's own Normal layouts
// (Player Setup > Edit Profile > Controls, read in a scripted Windows run 2026-11-02): left stick move forward/back
// and left/right, right stick look up/down and rotate the torso; RT and A Fire Weapon, LT Cycle Weapons, X Select
// Energy Weapon, Y Select Ballistic Weapon, B Select Missile Weapon, left stick click Jump, right stick click Defense,
// Back Start Menu, White Scoreboard, Black Toggle Global/Team Chat. Mouse aiming is src/game/mouse_aim.cpp.
// Settings: the [bindings] table of settings.toml, by control name, each an array of key names ("W", "Mouse1", ...).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "core/input.h"
#include "core/log.h"
#include "gpu/widescreen.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <imgui.h>
#include <toml++/toml.hpp>
#include "app/app.h"
#endif

namespace mechassault {
bool InPlay();  // src/game/campaign.cpp: a mission is being played (not a menu or the pause menu)
int PromptUnderCursor();  // src/game/prompts.cpp: the control whose button prompt is under the cursor, -1 none
int MenuHover();          // src/game/prompts.cpp: the d-pad direction that walks a menu's highlight to the cursor
}

namespace mechassault {

namespace {

enum Control : int {
    kA, kB, kX, kY, kBlack, kWhite, kLT, kRT,  // pad.analog order
    kStart, kBack, kL3, kR3,
    kDUp, kDDown, kDLeft, kDRight,
    kMoveUp, kMoveDown, kMoveLeft, kMoveRight,
    kLookUp, kLookDown, kLookLeft, kLookRight,
    kControls
};

struct ControlInfo {
    const char* key;    // settings name
    const char* label;  // F1 menu, the game's action in its Normal layout
};
constexpr ControlInfo kInfo[kControls] = {
    {"a", "Fire / menu select (A)"},
    {"b", "Select missile weapon / back (B)"},
    {"x", "Select energy weapon (X)"},
    {"y", "Select ballistic weapon (Y)"},
    {"black", "Toggle team chat (Black)"},
    {"white", "Scoreboard (White)"},
    {"left_trigger", "Cycle weapons (left trigger)"},
    {"right_trigger", "Fire weapon (right trigger)"},
    {"start", "Pause (Start)"},
    {"back", "Start menu (Back)"},
    {"left_thumb", "Jump jets (left stick click)"},
    {"right_thumb", "Defense (right stick click)"},
    {"dpad_up", "D-pad up"},
    {"dpad_down", "D-pad down"},
    {"dpad_left", "D-pad left"},
    {"dpad_right", "D-pad right"},
    {"move_forward", "Move forward"},
    {"move_back", "Move back"},
    {"move_left", "Move left"},
    {"move_right", "Move right"},
    // the right stick's up tilts the view down with the game's default Y axis (measured), so these name the stick
    {"look_up", "Right stick up (looks down)"},
    {"look_down", "Right stick down (looks up)"},
    {"look_left", "Rotate torso left"},
    {"look_right", "Rotate torso right"},
};

constexpr int kSlots = 2;

#ifdef _WIN32
// Windows virtual-key codes; the mouse buttons are VK_LBUTTON (1), VK_RBUTTON (2), VK_MBUTTON (4), VK_XBUTTON1/2 (5, 6)
// the mouse wheel as two keys on codes Windows leaves unassigned: each notch is a short press
constexpr uint8_t kWheelUp = 0x0E, kWheelDown = 0x0F;
constexpr uint8_t kDefaults[kControls][kSlots] = {
    {VK_RETURN, 0},           // A: fire too, and Enter confirms in menus
    {VK_BACK, '3'},           // B: missile weapon; Backspace is back in menus
    {'1', 0},                 // X: energy weapon
    {'2', 0},                 // Y: ballistic weapon
    {'T', 0},                 // Black: team chat
    {VK_TAB, 0},              // White: scoreboard
    {VK_RBUTTON, 'Q'},        // LT: cycle weapons
    {VK_LBUTTON, 0},          // RT: fire
    {VK_ESCAPE, 0},           // Start
    {'M', 0},                 // Back: start menu
    {VK_SPACE, 0},            // L3: jump jets (hold)
    {VK_LSHIFT, 'F'},         // R3: defense
    {VK_UP, kWheelUp},        // d-pad: menus, and the wheel moves through them
    {VK_DOWN, kWheelDown},
    {VK_LEFT, 0},
    {VK_RIGHT, 0},
    {'W', 0},
    {'S', 0},
    {'A', 0},
    {'D', 0},
    {'K', 0},  // stick up: looks down, so K
    {'I', 0},  // stick down: looks up, so I
    {'J', 0},
    {'L', 0},
};

std::atomic<uint8_t> g_bind[kControls][kSlots];

void ResetBindings() {
    for (int c = 0; c < kControls; c++)
        for (int s = 0; s < kSlots; s++) g_bind[c][s] = kDefaults[c][s];
}
const bool g_reset = (ResetBindings(), true);

// names in the settings file and the menu
struct KeyName {
    uint8_t vk;
    const char* name;
};
constexpr KeyName kNames[] = {
    {VK_LBUTTON, "Mouse1"},  {VK_RBUTTON, "Mouse2"},  {VK_MBUTTON, "Mouse3"},     {VK_XBUTTON1, "Mouse4"},
    {VK_XBUTTON2, "Mouse5"}, {VK_BACK, "Backspace"},  {VK_TAB, "Tab"},            {VK_RETURN, "Enter"},
    {VK_PAUSE, "Pause"},     {VK_CAPITAL, "CapsLock"}, {VK_ESCAPE, "Escape"},     {VK_SPACE, "Space"},
    {VK_PRIOR, "PageUp"},    {VK_NEXT, "PageDown"},   {VK_END, "End"},            {VK_HOME, "Home"},
    {VK_LEFT, "Left"},       {VK_UP, "Up"},           {VK_RIGHT, "Right"},        {VK_DOWN, "Down"},
    {VK_INSERT, "Insert"},   {VK_DELETE, "Delete"},   {VK_NUMPAD0, "Num0"},       {VK_NUMPAD1, "Num1"},
    {VK_NUMPAD2, "Num2"},    {VK_NUMPAD3, "Num3"},    {VK_NUMPAD4, "Num4"},       {VK_NUMPAD5, "Num5"},
    {VK_NUMPAD6, "Num6"},    {VK_NUMPAD7, "Num7"},    {VK_NUMPAD8, "Num8"},       {VK_NUMPAD9, "Num9"},
    {VK_MULTIPLY, "Num*"},   {VK_ADD, "Num+"},        {VK_SUBTRACT, "Num-"},      {VK_DECIMAL, "Num."},
    {VK_DIVIDE, "Num/"},     {VK_F4, "F4"},           {VK_F5, "F5"},              {VK_F6, "F6"},
    {VK_F7, "F7"},           {VK_F8, "F8"},           {VK_F9, "F9"},              {VK_F10, "F10"},
    {VK_F11, "F11"},         {VK_F12, "F12"},         {VK_LSHIFT, "LShift"},      {VK_RSHIFT, "RShift"},
    {VK_LCONTROL, "LCtrl"},  {VK_RCONTROL, "RCtrl"},  {VK_LMENU, "LAlt"},         {VK_RMENU, "RAlt"},
    {VK_OEM_1, ";"},         {VK_OEM_PLUS, "="},      {VK_OEM_COMMA, ","},        {VK_OEM_MINUS, "-"},
    {VK_OEM_PERIOD, "."},    {VK_OEM_2, "/"},         {VK_OEM_3, "`"},            {VK_OEM_4, "["},
    {VK_OEM_5, "\\"},        {VK_OEM_6, "]"},         {VK_OEM_7, "'"},
    {kWheelUp, "WheelUp"},   {kWheelDown, "WheelDown"},
};

// F1 to F3 belong to the settings menu, the FPS counter and frame capture; the plain Shift, Ctrl and Alt codes
// stand for either side, and their sided codes are offered instead
bool Bindable(int vk) {
    if (vk <= 0 || vk > 0xFE || vk == VK_F1 || vk == VK_F2 || vk == VK_F3) return false;
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN || vk == VK_CANCEL)
        return false;
    return true;
}

std::string Name(uint8_t vk) {
    if (!vk) return "";
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, char(vk));
    for (const KeyName& k : kNames)
        if (k.vk == vk) return k.name;
    char s[8];
    std::snprintf(s, sizeof(s), "VK%02X", vk);
    return s;
}

uint8_t Parse(const std::string& s) {
    if (s.size() == 1 && ((s[0] >= '0' && s[0] <= '9') || (s[0] >= 'A' && s[0] <= 'Z'))) return uint8_t(s[0]);
    if (s.size() == 1 && s[0] >= 'a' && s[0] <= 'z') return uint8_t(s[0] - 'a' + 'A');
    for (const KeyName& k : kNames)
        if (_stricmp(s.c_str(), k.name) == 0) return k.vk;
    unsigned v = 0;
    if (std::sscanf(s.c_str(), "VK%2X", &v) == 1 && Bindable(int(v))) return uint8_t(v);
    return 0;
}

// wheel notches waiting (+ up, - down), each played as a press of kHold ms and a gap of kGap ms, so that a game
// polling once a frame sees every one; the window thread adds them, the game's and the menu's threads read
std::mutex g_wheel_lock;
int g_wheel_queue = 0, g_wheel_rest = 0, g_wheel_dir = 0, g_wheel_seen = 0;
ULONGLONG g_wheel_until = 0;
constexpr ULONGLONG kHold = 80, kGap = 50;
constexpr int kMaxQueued = 3;

// the press playing now: 1 up, -1 down, 0 none
int WheelNow() {
    std::lock_guard l(g_wheel_lock);
    ULONGLONG now = GetTickCount64();
    if (g_wheel_dir && now < g_wheel_until) return g_wheel_dir;
    if (g_wheel_dir && now < g_wheel_until + kGap) return 0;
    g_wheel_dir = 0;
    if (g_wheel_queue) {
        g_wheel_dir = g_wheel_queue > 0 ? 1 : -1;
        g_wheel_queue -= g_wheel_dir;
        g_wheel_until = now + kHold;
    }
    return g_wheel_dir;
}

void ClearWheel() {
    std::lock_guard l(g_wheel_lock);
    g_wheel_queue = g_wheel_rest = 0;
}

// the last direction turned since the binding list began listening, once
int TakeWheelSeen() {
    std::lock_guard l(g_wheel_lock);
    int d = g_wheel_seen;
    g_wheel_seen = 0;
    return d;
}

bool Down(int vk) {
    if (vk == kWheelUp) return WheelNow() > 0;
    if (vk == kWheelDown) return WheelNow() < 0;
    return vk && (GetAsyncKeyState(vk) & 0x8000) != 0;
}
// in a menu the left and right mouse buttons are select and back, whatever they are bound to in play
bool g_menu = true;
bool Held(int c) {
    for (int s = 0; s < kSlots; s++) {
        uint8_t vk = g_bind[c][s];
        if (g_menu && (vk == VK_LBUTTON || vk == VK_RBUTTON)) continue;
        if (!g_menu && (vk == kWheelUp || vk == kWheelDown)) continue;  // in play the wheel cycles weapons
        if (Down(vk)) return true;
    }
    return false;
}

#endif

}  // namespace

// WM_MOUSEWHEEL (mouse_look.cpp passes it on): whole notches are queued, turning back drops the other way's
void OnWheel(int delta) {
#ifdef _WIN32
    std::lock_guard l(g_wheel_lock);
    g_wheel_rest += delta;
    int notches = g_wheel_rest / WHEEL_DELTA;
    g_wheel_rest -= notches * WHEEL_DELTA;
    if (!notches) return;
    // mechassault-recomp patch: in play each notch is a Cycle Weapons press (LT, which only goes forward through the
    // three weapon kinds): down one, up two, which comes back to the previous kind
    if (!g_menu) notches = notches > 0 ? notches * 2 : notches;
    if ((notches > 0) != (g_wheel_queue > 0)) g_wheel_queue = 0;
    g_wheel_queue = std::clamp(g_wheel_queue + notches, -kMaxQueued, kMaxQueued);
    g_wheel_seen = notches > 0 ? 1 : -1;
#else
    (void)delta;
#endif
}

// the first key bound to a control (A 0, B 1, X 2, Y 3, Start 8, Back 9: Control's order) and its name, for the
// button prompts (src/game/prompts.cpp); 0 and "" when unbound
uint8_t BoundVk(int control) {
#ifdef _WIN32
    if (control < 0 || control >= kControls) return 0;
    for (int s = 0; s < kSlots; s++)
        if (uint8_t vk = g_bind[control][s]) return vk;
#else
    (void)control;
#endif
    return 0;
}
std::string BoundName(int control) {
#ifdef _WIN32
    return Name(BoundVk(control));
#else
    (void)control;
    return "";
#endif
}

void KeyboardPad(int port, input::Pad& pad) {
#ifdef _WIN32
    if (port != 0) return;
    // menus: left click selects (A), right click goes back (B); in play they keep their bindings (fire, cycle weapons).
    // A change counts once it has held for 0.1 s: the word flickers for a frame as one screen gives way to the next
    {
        static bool pending = false;
        static std::chrono::steady_clock::time_point since;
        bool menu = !InPlay();
        auto now = std::chrono::steady_clock::now();
        if (menu == g_menu) {
            pending = false;
        } else if (!pending) {
            pending = true;
            since = now;
        } else if (now - since >= std::chrono::milliseconds(100)) {
            pending = false;
            g_menu = menu;
            gpu::wide::g_hud_learn = !menu;  // HUD pieces are learnt in play only (d3d12 HudShift)
            LOGI("input: %s", g_menu ? "menu (clicks select and go back)" : "play (clicks fire and cycle weapons)");
        }
    }
    if (app::MenuOpen()) ClearWheel();  // the wheel scrolls the menu, not the game behind it
    // the keyboard stays plugged in while another window has focus: no "reconnect the controller" pause on alt-tab
    pad.connected = true;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg || !GetWindowThreadProcessId(fg, &pid) || pid != GetCurrentProcessId()) return;
    // buttons: dpad up/down/left/right, start, back, left thumb, right thumb
    const int kButtons[] = {kDUp, kDDown, kDLeft, kDRight, kStart, kBack, kL3, kR3};
    for (int i = 0; i < 8; i++)
        if (Held(kButtons[i])) pad.buttons |= uint16_t(1u << i);
    // analog: A B X Y black white left trigger right trigger
    for (int i = 0; i < 8; i++)
        if (Held(kA + i)) pad.analog[i] = 0xFF;
    // a click on a button prompt (the 2 of "2 'Mech Stats") presses that button; anywhere else it is A
    static int clicked = -1;
    static bool was_down = false;
    bool down = Down(VK_LBUTTON);
    if (g_menu && down && !was_down) clicked = PromptUnderCursor();
    if (!down) clicked = -1;
    was_down = down;
    if (g_menu && down) {
        if (clicked == kStart || clicked == kBack) pad.buttons |= uint16_t(1u << (4 + clicked - kStart));
        else if (clicked >= kA && clicked <= kRT) pad.analog[clicked] = 0xFF;
        else pad.analog[kA] = 0xFF;
    }
    if (g_menu && Down(VK_RBUTTON)) pad.analog[kB] = 0xFF;
    // mechassault-recomp patch: the menus' highlight follows the mouse (src/game/prompts.cpp)
    if (g_menu && !app::MenuOpen())
        if (int h = MenuHover()) pad.buttons |= uint16_t(h < 0 ? 1u : 2u);  // d-pad up, down
    if (!g_menu && WheelNow()) pad.analog[kLT] = 0xFF;
    // sticks: full deflection, only where the real pad is centred
    auto axis = [](int minus, int plus) { return int16_t((Held(plus) ? 32767 : 0) - (Held(minus) ? 32767 : 0)); };
    if (int16_t v = axis(kMoveLeft, kMoveRight)) pad.lx = v;
    if (int16_t v = axis(kMoveDown, kMoveUp)) pad.ly = v;
    if (int16_t v = axis(kLookLeft, kLookRight)) pad.rx = v;
    if (int16_t v = axis(kLookDown, kLookUp)) pad.ry = v;
#else
    (void)port;
    (void)pad;
#endif
}

// settings.toml [bindings]: a control missing there keeps its default; an empty array leaves it unbound
void LoadBindings(const void* table) {
#ifdef _WIN32
    const toml::table& t = *static_cast<const toml::table*>(table);
    const toml::table* b = t["bindings"].as_table();
    if (!b) return;
    for (int c = 0; c < kControls; c++) {
        const toml::array* keys = (*b)[kInfo[c].key].as_array();
        if (!keys) continue;
        for (int s = 0; s < kSlots; s++) {
            uint8_t vk = 0;
            if (s < int(keys->size()))
                if (auto name = (*keys)[size_t(s)].value<std::string>()) vk = Parse(*name);
            g_bind[c][s] = vk;
        }
    }
#else
    (void)table;
#endif
}

void SaveBindings(void* table) {
#ifdef _WIN32
    toml::table b;
    for (int c = 0; c < kControls; c++) {
        toml::array keys;
        for (int s = 0; s < kSlots; s++)
            if (uint8_t vk = g_bind[c][s]) keys.push_back(Name(vk));
        b.insert(kInfo[c].key, std::move(keys));
    }
    static_cast<toml::table*>(table)->insert_or_assign("bindings", std::move(b));
#else
    (void)table;
#endif
}

// the F1 menu's Controls section, under the mouse settings: click a binding, then press a key or mouse button
void BindingsMenu() {
#ifdef _WIN32
    static int listen_control = -1, listen_slot = 0;
    static bool released = false;  // every key up since listening began, so the click that started it is not taken
    static double listen_start = 0;
    ImGui::SeparatorText("Keys and mouse buttons");
    // listening: the first key or mouse button pressed after all were released; nothing for 5 seconds cancels
    bool just_bound = false;  // a right-click that was just bound is not also a clear
    if (listen_control >= 0) {
        int pressed = 0;
        bool any = false;
        if (int wheel = TakeWheelSeen(); wheel && released) pressed = wheel > 0 ? kWheelUp : kWheelDown;
        for (int vk = 1; vk <= 0xFE; vk++) {
            if (vk == kWheelUp || vk == kWheelDown || !Bindable(vk) || !Down(vk)) continue;
            any = true;
            if (!pressed) pressed = vk;
        }
        if (!released) {
            released = !any;
        } else if (pressed) {
            g_bind[listen_control][listen_slot] = uint8_t(pressed);
            listen_control = -1;
            just_bound = true;
            app::Save();
        }
        if (listen_control >= 0 && ImGui::GetTime() - listen_start > 5) listen_control = -1;
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Click a binding, then press a key, mouse button or wheel. Right-click clears it.");
    ImGui::PopTextWrapPos();
    if (ImGui::BeginTable("##bindings", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Alternative", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (int c = 0; c < kControls; c++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(kInfo[c].label);
            for (int s = 0; s < kSlots; s++) {
                ImGui::TableNextColumn();
                ImGui::PushID(c * kSlots + s);
                bool listening = listen_control == c && listen_slot == s;
                std::string text = listening ? "press a key..." : Name(g_bind[c][s]);
                if (text.empty()) text = "-";
                if (ImGui::Button(text.c_str(), ImVec2(-FLT_MIN, 0)) && !listening) {
                    listen_control = c;
                    listen_slot = s;
                    released = false;
                    listen_start = ImGui::GetTime();
                    TakeWheelSeen();  // only a turn from now on
                }
                if (!just_bound && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    g_bind[c][s] = 0;
                    if (listening) listen_control = -1;
                    app::Save();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset to defaults")) {
        ResetBindings();
        listen_control = -1;
        app::Save();
    }
#endif
}

}  // namespace mechassault
