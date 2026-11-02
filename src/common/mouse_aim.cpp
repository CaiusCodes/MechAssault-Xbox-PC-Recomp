// Mouse aiming for MechAssault, first version: the mouse drives pad 1's right stick, which the game reads as Look
// up/down and Rotate left/right (the torso; its Normal thumbstick layout). The game turns the torso at a rate set by
// the stick, so each poll the mouse counts since the last one are turned into the deflection that would turn that far
// in the time since the last poll, past the stick's dead zone; what does not fit under full deflection carries into
// the next polls, so a quick flick finishes at the stick's top rate instead of being lost. A real pad's right stick
// wins. The window, raw input, cursor capture and the menu come from the Breakdown port's mouse look; Breakdown's
// version wrote into the game's own controller object, which MechAssault does not have mapped yet
// (KNOWN_ISSUES.md: mouse aiming is a stick emulation).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

#include "core/input.h"
#include "core/log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <imgui.h>
#include "app/app.h"
#endif

namespace mechassault {

void OnWheel(int delta);  // keyboard_pad.cpp: the wheel as d-pad presses
void BindingsMenu();      // keyboard_pad.cpp

// settings.toml: mouse_aim, mouse_sensitivity (percent; 100 = 0.05 degrees per count), mouse_invert_y
bool g_mouse_aim = true;
uint32_t g_mouse_sensitivity = 100;
bool g_mouse_invert_y = false;

namespace {

std::atomic<int32_t> g_dx{0}, g_dy{0};  // raw counts the game has not taken yet

// The stick model: full deflection turns the torso kFullRate degrees a second. The game's pad code (0x276C0) scales
// each axis by 1/32768, zeroes it inside a dead zone of 0.30 (0x33D90C) and stretches the rest back to 0..1
// ((|v| - 0.3) / 0.7), so a deflection just past 0.30 plus the share wanted gives that share of the top rate.
// kFullRate: measured from the HUD compass after holding the right stick fully right from rest (Mission 1, 60 fps):
// about 45 degrees after 0.5 s and 120-125 after 1 s, so about 150 degrees a second once turning, with a short
// ramp-up from rest the mouse's carry-over absorbs.
constexpr float kFullRate = 150.0f;
constexpr float kDeadZone = 0.302f;
constexpr float kMaxPending = 60.0f;  // degrees the mouse may run ahead of the torso

// the size of the deflection that turns the pending degrees this poll (at most full), taking them off pending
int16_t Deflection(float& pending, float dt) {
    if (std::fabs(pending) < 0.01f || dt <= 0) return 0;
    float use = std::min(std::fabs(pending) / (kFullRate * dt), 1.0f);  // share of the top rate this poll needs
    pending -= std::copysign(use * kFullRate * dt, pending);
    return int16_t((kDeadZone + use * (1.0f - kDeadZone)) * 32767.0f);
}

#ifdef _WIN32
bool g_captured = false;

void Capture(HWND hwnd) {
    if (g_captured) return;
    RECT r;
    GetClientRect(hwnd, &r);
    MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
    ClipCursor(&r);
    while (ShowCursor(FALSE) >= 0) {
    }
    g_captured = true;
}

void Release() {
    if (!g_captured) return;
    ClipCursor(nullptr);
    while (ShowCursor(TRUE) < 0) {
    }
    g_captured = false;
    g_dx = 0;
    g_dy = 0;
}
#endif

}  // namespace

// pad 1, after the keyboard: the mouse's share of the right stick
void MouseAimPad(int port, input::Pad& pad) {
    if (port != 0) return;
    using clock = std::chrono::steady_clock;
    static clock::time_point last = clock::now();
    static float pend_x = 0, pend_y = 0;
    auto now = clock::now();
    float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.0f, 0.1f);
    last = now;
    int32_t dx = g_dx.exchange(0), dy = g_dy.exchange(0);
    if (!g_mouse_aim) {
        pend_x = pend_y = 0;
        return;
    }
    float k = 0.05f * float(g_mouse_sensitivity) / 100.0f;
    pend_x = std::clamp(pend_x + float(dx) * k, -kMaxPending, kMaxPending);
    // the stick's up is positive: mouse up (negative counts) looks up unless inverted
    pend_y = std::clamp(pend_y - float(dy) * k * (g_mouse_invert_y ? -1.0f : 1.0f), -kMaxPending, kMaxPending);
    if (pad.rx == 0) {
        float before = pend_x;
        int16_t v = Deflection(pend_x, dt);
        if (v) pad.rx = before > 0 ? int16_t(std::abs(v)) : int16_t(-std::abs(v));
    } else {
        pend_x = 0;
    }
    if (pad.ry == 0) {
        float before = pend_y;
        int16_t v = Deflection(pend_y, dt);
        if (v) pad.ry = before > 0 ? int16_t(std::abs(v)) : int16_t(-std::abs(v));
    } else {
        pend_y = 0;
    }
}

// the game window is up: raw mouse input to it
void MouseWindow(void* hwnd) {
#ifdef _WIN32
    RAWINPUTDEVICE dev{0x01, 0x02, 0, static_cast<HWND>(hwnd)};  // generic desktop, mouse
    if (!RegisterRawInputDevices(&dev, 1, sizeof(dev))) LOGR("mouse: raw input not available (%lu)", GetLastError());
#else
    (void)hwnd;
#endif
}

// window thread: raw mouse counts while the window has the cursor; the cursor is held in the window while mouse aim is
// on, the window is in front and the F1 menu is closed
bool MouseMessage(void* hwnd_, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(hwnd_);
    (void)result;
    switch (msg) {
        case WM_INPUT: {
            bool want = g_mouse_aim && !app::MenuOpen() && GetForegroundWindow() == hwnd && !IsIconic(hwnd);
            if (!want) {
                Release();
                break;
            }
            RAWINPUT in;
            UINT size = sizeof(in);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &in, &size, sizeof(RAWINPUTHEADER)) == UINT(-1))
                break;
            if (in.header.dwType != RIM_TYPEMOUSE || (in.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) break;
            if (!g_captured) {
                Capture(hwnd);  // the first movement takes the cursor; that one is not a turn
                break;
            }
            g_dx += in.data.mouse.lLastX;
            g_dy += in.data.mouse.lLastY;
            break;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) Release();
            break;
        case WM_KEYDOWN:
            if (wp == VK_F1) Release();  // the settings menu opens: the cursor is the menu's
            break;
        case WM_MOUSEWHEEL:
            OnWheel(GET_WHEEL_DELTA_WPARAM(wp));  // d-pad presses (keyboard_pad.cpp); ImGui has had it already
            break;
        case WM_KILLFOCUS:
        case WM_DESTROY:
            Release();
            break;
        case WM_SIZE:
        case WM_MOVE:
            if (g_captured) {
                Release();
                Capture(hwnd);
            }
            break;
    }
#else
    (void)hwnd_, (void)msg, (void)wp, (void)lp, (void)result;
#endif
    return false;
}

// the F1 menu's Controls section
void MouseMenu() {
#ifdef _WIN32
    if (!ImGui::CollapsingHeader("Controls", ImGuiTreeNodeFlags_DefaultOpen)) return;
    bool changed = ImGui::Checkbox("Mouse aim", &g_mouse_aim);
    int sens = int(g_mouse_sensitivity);
    if (ImGui::SliderInt("Mouse sensitivity", &sens, 10, 400, "%d%%")) {
        g_mouse_sensitivity = uint32_t(sens);
        changed = true;
    }
    changed |= ImGui::Checkbox("Invert mouse Y", &g_mouse_invert_y);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("The mouse turns the torso and looks up and down through the game's right stick. The cursor "
                       "is held while the game is in front.");
    ImGui::PopStyleColor();
    if (changed) app::Save();
    BindingsMenu();
#endif
}

}  // namespace mechassault
